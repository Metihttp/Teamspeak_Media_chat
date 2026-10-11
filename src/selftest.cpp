// Test builds only: see selftest.h. Commands (one JSON object per line of selftest_script.txt):
//   wait {ms}                       pause
//   log {text}                      a marker line in TeamSpeak's log
//   upload {files}                  Core::uploadFiles to the visible chat (the 2.1 path)
//   send {files, caption, album, spoiler:[i...], quality:"original|auto|1080|720|480", repeat}
//                                   Core::send to the visible chat (repeat: that many sends right away)
//   compose {files, caption}        opens the send window for the visible chat (caption: prefilled)
//   compose.set {caption, album, spoiler:[i...], quality:[[i,q]...]}
//   compose.wait {timeout}          until its thumbnails are made
//   compose.grab {name}  compose.log  compose.send (presses Send)  compose.cancel
//   settings.grab {prefix}          opens Settings, grabs every tab, closes it
//   snap {name, all}                grabs the visible chat (all: every chat view)
//   grabmain {name}                 grabs TeamSpeak's main window
//   jobs  entries {match}           logs upload jobs / media entries
//   waitidle {timeout}              until no upload is preparing, compressing, uploading or posting
//   reveal {name}  play {name}  download {name}  viewer {name}
//   viewer.grab {name}  viewer.key {key: Space|Right|Left|Escape|Return}  viewer.close
//   clearcache  cancel  sendoriginal  dump  menus  windows
//   setting {key, value}            a few Settings values (see below), saved and applied
//   connect {nick}                  a new server tab to 127.0.0.1:9987
//   say {text}                      a raw channel message (no flood governor)
//   quit                            TeamSpeak's own Quit action (Ctrl+Q), queued
// Part 2 of the 2.2 live test:
//   react {name, reaction, emoji, times, gapMs}   toggles a reaction of the newest entry named so (PeerHub::toggle);
//                                   reaction: a v1 index 0..5, or emoji: any emoji (text or wire code)
//   reactmany {count, reaction, emoji} one toggle on each of the newest <count> media of this server, at once
//   reactions {name}                logs the reaction view of an entry
//   peers                           logs the presence summary of the visible chat and PeerHub's diagnostics
//   selfvar {var, value}  vars      sets / logs our client variables (CLIENT_INPUT_MUTED = 6, ...)
//   command {text}                  /tsmedia <text> (ts3plugin_processCommand)
//   menuitem {id, nick}             a plugin menu item (ts3plugin_onMenuItemEvent), on a client by nickname
//   settings.tab {name}             opens Settings on the tab whose title contains name
//   grabobj {object, name}          grabs a visible top-level window by objectName (prefix)
//   textdump {object, name}         writes the texts of a top-level window's text boxes to debug/<name>.txt
//   clipboard {name}                writes the clipboard text to debug/<name>.txt
//   toast {name}                    grabs the upload toast of the visible chat
//   click {..., object}             as above; also by window objectName, '&' ignored in button texts

#ifdef TSMEDIA_TESTHOOKS

#include "selftest.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QToolTip>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabBar>
#include <QTabWidget>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHash>
#include <QMouseEvent>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>

#include <functional>

#include "chatemoji.h"     // part 3: HD emoji counts
#include "chatinput.h"     // 2.2.1: TeamSpeak's placeholder in the chat input
#include "chatintegration.h"
#include "chatreactions.h" // part 3: the add-reaction button
#include "composedialog.h"
#include "emojiformat.h"   // part 3
#include "actionbar.h"     // part 4: the chat redesign's action bar
#include "chatlayout.h"    // part 4
#include "emojiinput.h"    // part 4: TeamSpeak's emoji button taken over
#include "layoutdoc.h"     // part 4: chips
#include "layoutformat.h"  // part 4
#include "micbutton.h"     // part 5: the chat input's microphone
#include "reactionart.h"   // part 3: rx::addButtonRect
#include "voicecontroller.h" // part 5
#include "voicepanel.h"      // part 5
#include "replydoc.h"      // part 3: messages and reply lines as the plugin reads them
#include "core.h"
#include "emojidata.h" // 2.2 emoji: reactions with any emoji
#include "inlinemedia.h"
#include "peerhub.h"
#include "settings.h"
#include "ts3api.h"

// plugin.cpp's entry points, called the way TeamSpeak calls them (same DLL).
extern "C" int  ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command);
extern "C" void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID);
extern "C" void ts3plugin_onHotkeyEvent(const char* keyword);

namespace {

QString withoutMnemonic(QString text)
{
    text.remove(QLatin1Char('&'));
    return text;
}

QWidget* topLevelByObject(const QString& prefix)
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->objectName().startsWith(prefix))
            return w;
    }
    return nullptr;
}

void writeDebugText(const QString& name, const QString& text)
{
    QDir().mkpath(ts3::dataDir() + QString::fromLatin1("/debug"));
    QFile file(ts3::dataDir() + QString::fromLatin1("/debug/") + name + QString::fromLatin1(".txt"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(text.toUtf8());
}

bool isLocal(uint64 sch)
{
    QString host;
    quint16 port = 0;
    return ts3::isConnected(sch) && ts3::getServerAddress(sch, &host, &port)
        && (host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1"));
}

void say(const QString& text)
{
    ts3::log(QString::fromLatin1("[test] script: ") + text);
}

// 2.2 emoji: a command's reaction as an emoji id. "emoji" is the emoji's text or wire code ("1f525");
// otherwise "reaction" is a v1 index (0 thumbs up .. 5 fire), as the part-2 scripts were written.
int reactionOf(const QJsonObject& c)
{
    const QString text = c.value(QString::fromLatin1("emoji")).toString();
    if (!text.isEmpty()) {
        const int byText = emoji::find(text);
        return byText >= 0 ? byText : emoji::fromWireCode(text.toLatin1());
    }
    return proto::legacyReactionEmoji(c.value(QString::fromLatin1("reaction")).toInt(0));
}

QString reactionName(int reaction)
{
    const int legacy = proto::legacyReactionIndex(reaction);
    const QString code = QString::fromLatin1(emoji::wireCode(reaction));
    return legacy >= 0 ? QString::fromLatin1("%1/%2").arg(legacy).arg(code) : code;
}

QString stateName(UploadState s)
{
    switch (s) {
    case UploadState::Preparing: return QString::fromLatin1("Preparing");
    case UploadState::Compressing: return QString::fromLatin1("Compressing");
    case UploadState::Uploading: return QString::fromLatin1("Uploading");
    case UploadState::Posting: return QString::fromLatin1("Posting");
    case UploadState::Done: return QString::fromLatin1("Done");
    case UploadState::Failed: return QString::fromLatin1("Failed");
    case UploadState::Canceled: return QString::fromLatin1("Canceled");
    }
    return QString::fromLatin1("?");
}

QString mediaStateName(MediaState s)
{
    switch (s) {
    case MediaState::Idle: return QString::fromLatin1("Idle");
    case MediaState::Queued: return QString::fromLatin1("Queued");
    case MediaState::Downloading: return QString::fromLatin1("Downloading");
    case MediaState::Ready: return QString::fromLatin1("Ready");
    case MediaState::Failed: return QString::fromLatin1("Failed");
    }
    return QString::fromLatin1("?");
}

QStringList stringList(const QJsonValue& v)
{
    QStringList out;
    if (v.isString())
        out << v.toString();
    for (const QJsonValue& item : v.toArray())
        out << item.toString();
    return out;
}

SendQuality qualityOf(const QString& q)
{
    if (q == QLatin1String("original"))
        return SendQuality::Original;
    if (q == QLatin1String("1080"))
        return SendQuality::P1080;
    if (q == QLatin1String("720"))
        return SendQuality::P720;
    if (q == QLatin1String("480"))
        return SendQuality::P480;
    return SendQuality::Auto;
}

QWidget* topLevelNamed(const QString& name)
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == name && w->isVisible())
            return w;
    }
    return nullptr;
}

QString describeWidgetTexts(QWidget* root)
{
    QStringList parts;
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->isVisible() && !label->text().trimmed().isEmpty())
            parts << QString::fromLatin1("label{") + label->text().simplified() + QLatin1Char('}');
    }
    for (QAbstractButton* button : root->findChildren<QAbstractButton*>()) {
        if (button->isVisible() && !button->text().isEmpty())
            parts << QString::fromLatin1("button{%1%2%3}")
                         .arg(button->text(), button->isEnabled() ? QString() : QString::fromLatin1(" disabled"),
                              button->isCheckable() ? (button->isChecked() ? QString::fromLatin1(" on") : QString::fromLatin1(" off")) : QString());
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace

SelfTest::SelfTest(Core* core, ChatIntegration* chat, Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_chat(chat)
    , m_hooks(std::move(hooks))
{
    m_poll = new QTimer(this);
    m_poll->setInterval(1000);
    connect(m_poll, &QTimer::timeout, this, &SelfTest::poll);
    m_poll->start();
    m_step = new QTimer(this);
    m_step->setSingleShot(true);
    connect(m_step, &QTimer::timeout, this, &SelfTest::runOne);
}

void SelfTest::poll()
{
    if (m_running)
        return;
    const QString path = ts3::dataDir() + QString::fromLatin1("/selftest_script.txt");
    if (!QFile::exists(path))
        return;
    if (!isLocal(ts3::currentConnection()))
        return; // waits (the file stays) until the current tab is a localhost server
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    file.remove();
    m_lines.clear();
    for (const QString& line : text.split(QRegularExpression(QString::fromLatin1("[\r\n]+")), Qt::SkipEmptyParts)) {
        if (!line.trimmed().isEmpty() && !line.trimmed().startsWith(QLatin1Char('#')))
            m_lines << line.trimmed();
    }
    m_pos     = 0;
    m_running = true;
    say(QString::fromLatin1("start (%1 commands)").arg(m_lines.size()));
    next(0);
}

void SelfTest::next(int delayMs)
{
    m_step->start(qMax(0, delayMs));
}

void SelfTest::finish(const QString& why)
{
    m_running = false;
    m_lines.clear();
    say(QString::fromLatin1("end (") + why + QLatin1Char(')'));
}

void SelfTest::runOne()
{
    if (!m_running)
        return;
    if (m_pos >= m_lines.size()) {
        finish(QString::fromLatin1("done"));
        return;
    }
    if (!isLocal(ts3::currentConnection())) {
        // A tab that is still connecting (e.g. right after "connect") gets 20 s.
        if (m_notLocalSince == 0)
            m_notLocalSince = QDateTime::currentMSecsSinceEpoch();
        if (QDateTime::currentMSecsSinceEpoch() - m_notLocalSince < 20000) {
            next(500);
            return;
        }
        finish(QString::fromLatin1("stopped: the current tab is not a localhost server"));
        return;
    }
    m_notLocalSince = 0;
    const QString       line = m_lines.at(m_pos++);
    QJsonParseError     error;
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (!doc.isObject()) {
        say(QString::fromLatin1("bad line %1: %2").arg(m_pos).arg(error.errorString()));
        next(0);
        return;
    }
    say(QString::fromLatin1("> ") + line.left(400));
    if (run(doc.object()))
        next(0);
}

QString SelfTest::findKey(const QString& namePart) const
{
    const QStringList keys = m_core->keys();
    for (int i = keys.size() - 1; i >= 0; --i) {
        const MediaEntry* e = m_core->entry(keys.at(i));
        if (e && e->link.fileName.contains(namePart, Qt::CaseInsensitive))
            return keys.at(i);
    }
    return QString();
}

QString SelfTest::saveGrab(QWidget* widget, const QString& name) const
{
    if (!widget)
        return QString();
    const QString dir = ts3::dataDir() + QString::fromLatin1("/debug");
    QDir().mkpath(dir);
    QString safe = name;
    safe.replace(QRegularExpression(QString::fromLatin1("[^A-Za-z0-9_.-]")), QString::fromLatin1("_"));
    const QString path = dir + QLatin1Char('/') + safe + QString::fromLatin1(".png");
    widget->grab().save(path);
    return path;
}

void SelfTest::logJobs() const
{
    for (int id : m_core->uploadIds()) {
        const UploadJob* j = m_core->upload(id);
        if (!j)
            continue;
        say(QString::fromLatin1("job %1 batch %2 %3%4 %5 -> %6 size %7 album %8 spoiler %9 compression %10 \"%11\"")
                .arg(QString::number(j->id), QString::number(j->batch), stateName(j->state), j->waiting ? QString::fromLatin1(" (waiting)") : QString(),
                     QFileInfo(j->sourcePath).fileName(), j->remoteDir + QLatin1Char('/') + j->remoteName, QString::number(j->size),
                     QString::number(j->inAlbum ? 1 : 0), QString::number(j->spoiler ? 1 : 0))
                .arg(static_cast<int>(j->compression))
                .arg(j->message.left(300)));
    }
}

void SelfTest::logEntries(const QString& match) const
{
    for (const QString& key : m_core->keys()) {
        const MediaEntry* e = m_core->entry(key);
        if (!e || (!match.isEmpty() && !e->link.fileName.contains(match, Qt::CaseInsensitive)))
            continue;
        say(QString::fromLatin1("entry %1 kind %2 state %3 err %4 own %5 sha %6 verified %7 received %8 sp %9 hidden %10 album %11/%12 preview %13 \"%14\"")
                .arg(e->link.fileName, QString::number(static_cast<int>(e->kind)), mediaStateName(e->state), QString::number(static_cast<int>(e->error)),
                     QString::number(e->isOwnUpload ? 1 : 0), QString::fromLatin1(e->link.sha256.toHex().left(16)), QString::number(e->check.verified ? 1 : 0),
                     QString::fromLatin1(e->check.received.toHex().left(16)), QString::number(e->link.spoiler ? 1 : 0))
                .arg(m_core->isSpoilerHidden(key) ? 1 : 0)
                .arg(e->link.albumIndex)
                .arg(e->link.albumCount)
                .arg(mediaStateName(e->previewState))
                .arg(e->errorText.left(200)));
    }
}

void SelfTest::waitIdle(qint64 deadline)
{
    bool busy = false;
    for (int id : m_core->uploadIds()) {
        const UploadJob* j = m_core->upload(id);
        if (j && (j->state == UploadState::Preparing || j->state == UploadState::Compressing || j->state == UploadState::Uploading || j->state == UploadState::Posting))
            busy = true;
    }
    if (busy && QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (m_logJobsEveryMs > 0 && QDateTime::currentMSecsSinceEpoch() - m_jobsLoggedAt >= m_logJobsEveryMs) {
            m_jobsLoggedAt = QDateTime::currentMSecsSinceEpoch();
            for (int id : m_core->uploadIds()) {
                const UploadJob* j = m_core->upload(id);
                if (j && j->state != UploadState::Done && j->state != UploadState::Failed && j->state != UploadState::Canceled)
                    say(QString::fromLatin1("progress job %1 %2%3 %4% label %5 encoder %6 finishing %7 size %8/%9 \"%10\"")
                            .arg(QString::number(j->id), stateName(j->state), j->waiting ? QString::fromLatin1(" (waiting)") : QString(),
                                 QString::number(qRound(j->progress * 100)), j->compressLabel, QString::number(j->compressEncoder),
                                 QString::number(j->compressFinishing ? 1 : 0), QString::number(j->size), QString::number(j->originalSize))
                            .arg(j->message.left(160)));
            }
        }
        auto*              t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t, deadline] {
            t->deleteLater();
            waitIdle(deadline);
        });
        t->start(500);
        return;
    }
    m_logJobsEveryMs = 0;
    say(busy ? QString::fromLatin1("waitidle: timed out") : QString::fromLatin1("waitidle: idle"));
    logJobs();
    next(0);
}

void SelfTest::waitCompose(qint64 deadline)
{
    auto* dialog = qobject_cast<ComposeDialog*>(topLevelNamed(QString::fromLatin1("tsmediaCompose")));
    if (dialog && dialog->isBusy() && QDateTime::currentMSecsSinceEpoch() < deadline) {
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t, deadline] {
            t->deleteLater();
            waitCompose(deadline);
        });
        t->start(300);
        return;
    }
    say(dialog ? (dialog->isBusy() ? QString::fromLatin1("compose.wait: still busy") : QString::fromLatin1("compose.wait: ready")) : QString::fromLatin1("compose.wait: no send window"));
    next(0);
}

// Opens Settings (tab < 0), then grabs tab after tab, then closes it.
void SelfTest::settingsStep(const QString& prefix, int tab)
{
    QWidget* dialog = m_hooks.settingsDialog ? m_hooks.settingsDialog() : nullptr;
    if (tab < 0) {
        if (!dialog && m_hooks.openSettings)
            m_hooks.openSettings();
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t, prefix] {
            t->deleteLater();
            settingsStep(prefix, 0);
        });
        t->start(1200);
        return;
    }
    auto* tabs = dialog ? dialog->findChild<QTabWidget*>(QString::fromLatin1("tsmediaSettingsTabs")) : nullptr;
    if (!dialog || !tabs) {
        say(QString::fromLatin1("settings.grab: no settings dialog"));
        next(0);
        return;
    }
    if (tab > 0) { // the tab chosen in the previous step has been painted by now
        const int     shown = tab - 1;
        const QString path  = saveGrab(dialog, QString::fromLatin1("%1_%2_%3").arg(prefix).arg(shown).arg(tabs->tabText(shown)));
        say(QString::fromLatin1("settings.grab: tab %1 \"%2\" %3x%4 -> %5").arg(shown).arg(tabs->tabText(shown)).arg(dialog->width()).arg(dialog->height()).arg(path));
    }
    if (tab >= tabs->count()) {
        say(QString::fromLatin1("settings.grab: %1 tabs; dialog \"%2\"").arg(tabs->count()).arg(dialog->windowTitle()));
        if (auto* d = qobject_cast<QDialog*>(dialog))
            d->reject();
        else
            dialog->close();
        next(500);
        return;
    }
    tabs->setCurrentIndex(tab);
    auto* t = new QTimer(this);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, this, [this, t, prefix, tab] {
        t->deleteLater();
        settingsStep(prefix, tab + 1);
    });
    t->start(500);
}

bool SelfTest::run(const QJsonObject& c)
{
    const QString cmd = c.value(QString::fromLatin1("cmd")).toString();
    const uint64  sch = ts3::currentConnection();

    if (cmd == QLatin1String("wait")) {
        next(c.value(QString::fromLatin1("ms")).toInt(1000));
        return false;
    }
    if (cmd == QLatin1String("log")) {
        say(QString::fromLatin1("marker ") + c.value(QString::fromLatin1("text")).toString());
        return true;
    }
    if (!m_chat) {
        say(QString::fromLatin1("no chat integration"));
        return true;
    }
    if (cmd == QLatin1String("upload")) {
        m_core->uploadFiles(stringList(c.value(QString::fromLatin1("files"))), m_chat->currentTarget());
        return true;
    }
    if (cmd == QLatin1String("send")) {
        SendRequest request;
        request.target  = m_chat->currentTarget();
        request.caption = c.value(QString::fromLatin1("caption")).toString();
        request.album   = c.value(QString::fromLatin1("album")).toBool(false);
        QSet<int> spoilers;
        for (const QJsonValue& v : c.value(QString::fromLatin1("spoiler")).toArray())
            spoilers.insert(v.toInt());
        const SendQuality quality = qualityOf(c.value(QString::fromLatin1("quality")).toString());
        const QStringList files   = stringList(c.value(QString::fromLatin1("files")));
        for (int i = 0; i < files.size(); ++i) {
            SendItem item;
            item.path    = files.at(i);
            item.spoiler = spoilers.contains(i);
            item.quality = quality;
            request.items.append(item);
        }
        const int repeat = qMax(1, c.value(QString::fromLatin1("repeat")).toInt(1));
        for (int r = 0; r < repeat; ++r) {
            SendRequest copy = request;
            if (repeat > 1 && !copy.caption.isEmpty())
                copy.caption += QString::fromLatin1(" #%1").arg(r + 1);
            const int batch = m_core->send(copy);
            say(QString::fromLatin1("send: batch %1 (%2 items, caption %3 chars, album %4)").arg(batch).arg(copy.items.size()).arg(copy.caption.size()).arg(copy.album ? 1 : 0));
        }
        return true;
    }
    if (cmd == QLatin1String("compose")) {
        QWidget* source = m_chat->visibleChatBrowser();
        for (const auto& input : m_chat->m_inputs) {
            if (input && input->isVisible()) {
                source = input;
                break;
            }
        }
        m_chat->openCompose(source, stringList(c.value(QString::fromLatin1("files"))), QImage(), m_chat->currentTarget(), c.value(QString::fromLatin1("caption")).toString());
        say(QString::fromLatin1("compose: open %1").arg(topLevelNamed(QString::fromLatin1("tsmediaCompose")) ? 1 : 0));
        return true;
    }
    if (cmd.startsWith(QLatin1String("compose."))) {
        auto* dialog = qobject_cast<ComposeDialog*>(topLevelNamed(QString::fromLatin1("tsmediaCompose")));
        if (!dialog) {
            say(cmd + QString::fromLatin1(": no send window"));
            return true;
        }
        if (cmd == QLatin1String("compose.set")) {
            if (c.contains(QString::fromLatin1("caption"))) {
                QLineEdit* field = dialog->captionField();
                field->setFocus();
                field->selectAll();
                field->insert(c.value(QString::fromLatin1("caption")).toString()); // as if typed: textEdited fires
            }
            if (c.contains(QString::fromLatin1("album")))
                dialog->setAlbum(c.value(QString::fromLatin1("album")).toBool());
            for (const QJsonValue& v : c.value(QString::fromLatin1("spoiler")).toArray())
                dialog->setSpoiler(v.toInt(), true);
            for (const QJsonValue& v : c.value(QString::fromLatin1("quality")).toArray())
                dialog->setQualityIndex(v.toArray().at(0).toInt(), v.toArray().at(1).toInt());
            say(QString::fromLatin1("compose.set: %1 items").arg(dialog->itemCount()));
            return true;
        }
        if (cmd == QLatin1String("compose.wait")) {
            waitCompose(QDateTime::currentMSecsSinceEpoch() + c.value(QString::fromLatin1("timeout")).toInt(20000));
            return false;
        }
        if (cmd == QLatin1String("compose.grab")) {
            say(QString::fromLatin1("compose.grab: %1x%2 -> %3").arg(dialog->width()).arg(dialog->height()).arg(saveGrab(dialog, c.value(QString::fromLatin1("name")).toString())));
            return true;
        }
        if (cmd == QLatin1String("compose.log")) {
            say(QString::fromLatin1("compose.log: \"%1\" items %2 caption \"%3\" %4")
                    .arg(dialog->windowTitle(), QString::number(dialog->itemCount()), dialog->captionField()->text(), describeWidgetTexts(dialog)));
            for (int i = 0; i < dialog->itemCount(); ++i) {
                QStringList q;
                for (const videocompress::Choice& choice : dialog->qualityChoices(i))
                    q << choice.label;
                say(QString::fromLatin1("compose.item %1: %2 quality [%3]").arg(i).arg(QFileInfo(dialog->itemAt(i).path).fileName(), q.join(QString::fromLatin1(", "))));
            }
            return true;
        }
        if (cmd == QLatin1String("compose.send")) {
            auto* box = dialog->findChild<QDialogButtonBox*>();
            for (QAbstractButton* b : box ? box->buttons() : QList<QAbstractButton*>()) {
                if (box->buttonRole(b) == QDialogButtonBox::AcceptRole) {
                    say(QString::fromLatin1("compose.send: \"%1\" enabled %2").arg(b->text()).arg(b->isEnabled() ? 1 : 0));
                    if (b->isEnabled())
                        b->click();
                    return true;
                }
            }
            say(QString::fromLatin1("compose.send: no Send button"));
            return true;
        }
        if (cmd == QLatin1String("compose.cancel")) {
            dialog->reject();
            return true;
        }
    }
    if (cmd == QLatin1String("settings.open")) {
        if (m_hooks.openSettings)
            m_hooks.openSettings();
        return true;
    }
    if (cmd == QLatin1String("settings.grab")) {
        settingsStep(c.value(QString::fromLatin1("prefix")).toString(QString::fromLatin1("settings")), -1);
        return false;
    }
    if (cmd == QLatin1String("snap")) {
        const QString name = c.value(QString::fromLatin1("name")).toString(QString::fromLatin1("snap"));
        if (c.value(QString::fromLatin1("all")).toBool()) {
            int n = 0;
            for (auto it = m_chat->m_views.begin(); it != m_chat->m_views.end(); ++it) {
                QTextBrowser* b = it->browser;
                if (!b)
                    continue;
                say(QString::fromLatin1("snap: view %1 visible %2 %3x%4 blocks %5 -> %6")
                        .arg(n)
                        .arg(b->isVisible() ? 1 : 0)
                        .arg(b->width())
                        .arg(b->height())
                        .arg(b->document()->blockCount())
                        .arg(saveGrab(b, QString::fromLatin1("%1_%2").arg(name).arg(n))));
                ++n;
            }
        } else {
            QTextBrowser* b = m_chat->visibleChatBrowser();
            say(QString::fromLatin1("snap: %1").arg(b ? saveGrab(b, name) : QString::fromLatin1("no visible chat")));
        }
        return true;
    }
    if (cmd == QLatin1String("tab")) { // a chat tab or a server tab of the main window, by its text
        const QString name  = c.value(QString::fromLatin1("name")).toString();
        bool          found = false;
        const int index = c.value(QString::fromLatin1("index")).toInt(0); // the n-th match (server tabs share names)
        int       seen  = 0;
        if (QWidget* mw = m_chat->mainWindow()) {
            const bool serverTabs = c.value(QString::fromLatin1("server")).toBool(false); // only the server tab bar
            const bool chatTabs   = c.value(QString::fromLatin1("chat")).toBool(false);   // only the chat tab bar (part 5)
            for (QTabBar* bar : mw->findChildren<QTabBar*>()) {
                if (!bar->isVisible())
                    continue; // a hidden server tab's chat tabs
                if (serverTabs && bar->objectName() == QLatin1String("ChatTabBar"))
                    continue;
                if (chatTabs && bar->objectName() != QLatin1String("ChatTabBar"))
                    continue;
                for (int i = 0; i < bar->count() && !found; ++i) {
                    if (bar->tabText(i).contains(name, Qt::CaseInsensitive) && seen++ < index)
                        continue;
                    if (bar->tabText(i).contains(name, Qt::CaseInsensitive)) {
                        bar->setCurrentIndex(i);
                        found = true;
                        say(QString::fromLatin1("tab: %1 \"%2\" selected").arg(bar->objectName(), bar->tabText(i)));
                    }
                }
            }
        }
        if (!found)
            say(QString::fromLatin1("tab: none named ") + name);
        next(600);
        return false;
    }
    if (cmd == QLatin1String("style")) { // test builds: a TeamSpeak skin's style sheet for this session
        static QString saved;               // Qt-allocated text (read from TeamSpeak), never a literal
        static bool    swapped = false;
        if (c.value(QString::fromLatin1("restore")).toBool()) {
            if (swapped) {
                qApp->setStyleSheet(saved);
                swapped = false;
                saved   = QString();
            }
            say(QString::fromLatin1("style: restored"));
        } else {
            QFile file(c.value(QString::fromLatin1("file")).toString());
            if (file.open(QIODevice::ReadOnly)) {
                if (!swapped) {
                    saved   = qApp->styleSheet();
                    swapped = true;
                }
                qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
                say(QString::fromLatin1("style: %1 applied (TeamSpeak's own: %2 chars, main window %3 chars)").arg(file.fileName()).arg(saved.size()).arg(m_chat->mainWindow() ? m_chat->mainWindow()->styleSheet().size() : -1));
            } else {
                say(QString::fromLatin1("style: can't read ") + file.fileName());
            }
        }
        next(800);
        return false;
    }
    if (cmd == QLatin1String("snapkey")) { // scrolls the visible chat to a file's preview, then grabs it
        QTextBrowser* b    = m_chat->visibleChatBrowser();
        const QString key  = findKey(c.value(QString::fromLatin1("name")).toString());
        const QString out  = c.value(QString::fromLatin1("out")).toString(QString::fromLatin1("snapkey"));
        auto*         view = b ? m_chat->viewFor(b) : nullptr;
        if (!view || key.isEmpty()) {
            say(QString::fromLatin1("snapkey: no chat or no entry for ") + c.value(QString::fromLatin1("name")).toString());
            return true;
        }
        m_chat->ensurePositions(*view);
        int position = view->positionsByKey.value(key).isEmpty() ? -1 : view->positionsByKey.value(key).first();
        if (position < 0) {
            for (const QString& album : view->albumsByKey.value(key)) {
                for (const auto& pp : qAsConst(view->previews)) {
                    if (pp.key == album) {
                        position = pp.position;
                        break;
                    }
                }
                if (position >= 0)
                    break;
            }
        }
        if (position < 0) {
            say(QString::fromLatin1("snapkey: %1 has no preview in the visible chat").arg(m_core->entry(key)->link.fileName));
            return true;
        }
        const QTextBlock block = b->document()->findBlock(position);
        const QRectF     rect  = b->document()->documentLayout()->blockBoundingRect(block);
        b->verticalScrollBar()->setValue(qMax(0, static_cast<int>(rect.top()) - 30));
        QPointer<QTextBrowser> guard(b);
        auto*                  t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t, guard, out] {
            t->deleteLater();
            say(QString::fromLatin1("snapkey: %1").arg(guard ? saveGrab(guard.data(), out) : QString::fromLatin1("chat gone")));
            if (guard)
                guard->verticalScrollBar()->setValue(guard->verticalScrollBar()->maximum());
            next(0);
        });
        t->start(900);
        return false;
    }
    if (cmd == QLatin1String("grabmain")) {
        say(QString::fromLatin1("grabmain: %1").arg(saveGrab(m_chat->mainWindow(), c.value(QString::fromLatin1("name")).toString(QString::fromLatin1("main")))));
        return true;
    }
    if (cmd == QLatin1String("jobs")) {
        logJobs();
        return true;
    }
    if (cmd == QLatin1String("entries")) {
        logEntries(c.value(QString::fromLatin1("match")).toString());
        return true;
    }
    if (cmd == QLatin1String("waitidle")) {
        m_logJobsEveryMs = c.value(QString::fromLatin1("logEvery")).toInt(0); // progress lines meanwhile
        m_jobsLoggedAt   = 0;
        waitIdle(QDateTime::currentMSecsSinceEpoch() + c.value(QString::fromLatin1("timeout")).toInt(120000));
        return false;
    }
    if (cmd == QLatin1String("reveal") || cmd == QLatin1String("play") || cmd == QLatin1String("download") || cmd == QLatin1String("viewer")) {
        const QString key = findKey(c.value(QString::fromLatin1("name")).toString());
        if (key.isEmpty()) {
            say(cmd + QString::fromLatin1(": no entry for ") + c.value(QString::fromLatin1("name")).toString());
            return true;
        }
        say(cmd + QString::fromLatin1(": ") + m_core->entry(key)->link.fileName);
        if (cmd == QLatin1String("reveal"))
            m_chat->revealSpoiler(key);
        else if (cmd == QLatin1String("play") && m_chat->media())
            m_chat->media()->click(key, VideoZone::Body, 0.0);
        else if (cmd == QLatin1String("download"))
            m_core->download(key, false);
        else if (cmd == QLatin1String("viewer"))
            m_chat->openViewer(key);
        return true;
    }
    if (cmd.startsWith(QLatin1String("viewer."))) {
        QWidget* viewer = topLevelNamed(QString::fromLatin1("tsmediaMediaViewer"));
        if (!viewer) {
            say(cmd + QString::fromLatin1(": no viewer"));
            return true;
        }
        if (cmd == QLatin1String("viewer.grab")) {
            say(QString::fromLatin1("viewer.grab: \"%1\" %2x%3 -> %4")
                    .arg(viewer->windowTitle())
                    .arg(viewer->width())
                    .arg(viewer->height())
                    .arg(saveGrab(viewer, c.value(QString::fromLatin1("name")).toString(QString::fromLatin1("viewer")))));
        } else if (cmd == QLatin1String("viewer.key")) {
            const QString name = c.value(QString::fromLatin1("key")).toString();
            int           key  = Qt::Key_Space;
            if (name == QLatin1String("Right"))
                key = Qt::Key_Right;
            else if (name == QLatin1String("Left"))
                key = Qt::Key_Left;
            else if (name == QLatin1String("Escape"))
                key = Qt::Key_Escape;
            else if (name == QLatin1String("Return"))
                key = Qt::Key_Return;
            QWidget*  target = QApplication::focusWidget() && QApplication::focusWidget()->window() == viewer ? QApplication::focusWidget() : viewer;
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QApplication::sendEvent(target, &press);
            QApplication::sendEvent(target, &release);
        } else if (cmd == QLatin1String("viewer.close")) {
            viewer->close();
        }
        return true;
    }
    if (cmd == QLatin1String("clearcache")) {
        m_core->clearCache();
        return true;
    }
    if (cmd == QLatin1String("cancel")) {
        for (int id : m_core->uploadIds()) {
            if (m_core->canCancelUpload(id))
                m_core->cancelUpload(id);
        }
        return true;
    }
    if (cmd == QLatin1String("sendoriginal")) {
        for (int id : m_core->uploadIds()) {
            if (m_core->canSendOriginal(id)) {
                say(QString::fromLatin1("sendoriginal: job %1").arg(id));
                m_core->sendOriginal(id);
            }
        }
        return true;
    }
    if (cmd == QLatin1String("dump")) {
        QFile file(ts3::dataDir() + QString::fromLatin1("/debug/widget_dump.txt"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(m_chat->dumpWidgetTree().toUtf8());
        say(QString::fromLatin1("dump: written"));
        return true;
    }
    if (cmd == QLatin1String("action")) { // triggers a main-window action by its text (queued), e.g. "Options"
        const QString text  = c.value(QString::fromLatin1("text")).toString();
        QAction*      found = nullptr;
        if (QWidget* mw = m_chat->mainWindow()) {
            for (QAction* a : mw->findChildren<QAction*>()) {
                QString t = a->text();
                t.remove(QLatin1Char('&'));
                if (t.compare(text, Qt::CaseInsensitive) == 0) {
                    found = a;
                    break;
                }
            }
        }
        say(QString::fromLatin1("action: %1").arg(found ? found->text() + QString::fromLatin1(" (") + found->shortcut().toString() + QLatin1Char(')') : QString::fromLatin1("not found")));
        if (found)
            QMetaObject::invokeMethod(found, "trigger", Qt::QueuedConnection);
        next(1500);
        return false;
    }
    if (cmd == QLatin1String("dumpui")) { // visible windows with the texts of their widgets
        QString out;
        std::function<void(QWidget*, int)> walk = [&](QWidget* w, int depth) {
            if (!w->isVisible())
                return;
            QString line = QString(depth * 2, QLatin1Char(' ')) + QString::fromLatin1(w->metaObject()->className());
            if (!w->objectName().isEmpty())
                line += QLatin1Char('#') + w->objectName();
            if (auto* b = qobject_cast<QAbstractButton*>(w))
                line += QString::fromLatin1(" text=\"%1\"%2").arg(b->text(), b->isCheckable() ? (b->isChecked() ? QString::fromLatin1(" [x]") : QString::fromLatin1(" [ ]")) : QString());
            else if (auto* l = qobject_cast<QLabel*>(w))
                line += QString::fromLatin1(" text=\"%1\"").arg(l->text().left(120));
            else if (auto* bar = qobject_cast<QTabBar*>(w)) {
                QStringList tabs;
                for (int i = 0; i < bar->count(); ++i)
                    tabs << bar->tabText(i);
                line += QString::fromLatin1(" tabs=") + tabs.join(QLatin1Char('|'));
            } else if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
                QStringList rows;
                QAbstractItemModel* model = view->model();
                std::function<void(const QModelIndex&, int)> rowsOf = [&](const QModelIndex& parent, int level) {
                    for (int r = 0; model && r < model->rowCount(parent) && rows.size() < 80; ++r) {
                        const QModelIndex index = model->index(r, 0, parent);
                        QStringList cols;
                        for (int col = 0; col < model->columnCount(parent) && col < 4; ++col)
                            cols << model->index(r, col, parent).data().toString() + (model->index(r, col, parent).data(Qt::CheckStateRole).isValid() ? QString::fromLatin1("{check=%1}").arg(model->index(r, col, parent).data(Qt::CheckStateRole).toInt()) : QString());
                        rows << QString(level, QLatin1Char('>')) + cols.join(QLatin1Char('/'));
                        rowsOf(index, level + 1);
                    }
                };
                rowsOf(QModelIndex(), 0);
                line += QString::fromLatin1(" rows=[") + rows.join(QString::fromLatin1("; ")) + QLatin1Char(']');
            }
            out += line + QLatin1Char('\n');
            for (QObject* child : w->children()) {
                if (auto* cw = qobject_cast<QWidget*>(child)) {
                    if (!cw->isWindow())
                        walk(cw, depth + 1);
                }
            }
        };
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (top->isVisible() && top != m_chat->mainWindow())
                walk(top, 0);
        }
        QDir().mkpath(ts3::dataDir() + QString::fromLatin1("/debug"));
        QFile file(ts3::dataDir() + QString::fromLatin1("/debug/") + c.value(QString::fromLatin1("name")).toString(QString::fromLatin1("ui_dump")) + QString::fromLatin1(".txt"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(out.toUtf8());
        say(QString::fromLatin1("dumpui: %1 lines").arg(out.count(QLatin1Char('\n'))));
        return true;
    }
    if (cmd == QLatin1String("click")) {
        // A button of a visible TeamSpeak window (e.g. Options > Addons > "Reload" of our plugin), clicked
        // through a queued call: TeamSpeak may unload this DLL in that click, so none of our code may be on
        // the stack then.
        const QString title = c.value(QString::fromLatin1("window")).toString();
        const QString text  = c.value(QString::fromLatin1("text")).toString();
        const QString frame = c.value(QString::fromLatin1("frameLabel")).toString();
        const QString object = c.value(QString::fromLatin1("object")).toString();
        QAbstractButton* target = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (!top->isVisible())
                continue;
            if (!object.isEmpty() ? !top->objectName().startsWith(object) : !top->windowTitle().contains(title, Qt::CaseInsensitive))
                continue;
            for (QAbstractButton* b : top->findChildren<QAbstractButton*>()) {
                if (!b->isVisible() || withoutMnemonic(b->text()) != withoutMnemonic(text))
                    continue;
                if (!frame.isEmpty()) {
                    bool inFrame = false;
                    for (QLabel* l : b->parentWidget() ? b->parentWidget()->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly) : QList<QLabel*>())
                        inFrame = inFrame || l->text().contains(frame);
                    if (!inFrame)
                        continue;
                }
                target = b;
                break;
            }
            if (target)
                break;
        }
        say(QString::fromLatin1("click: \"%1\" in \"%2\"%3%4")
                .arg(text, object.isEmpty() ? title : object, target ? QString() : QString::fromLatin1(" not found"),
                     target && !target->isEnabled() ? QString::fromLatin1(" (disabled)") : QString()));
        if (target)
            QMetaObject::invokeMethod(target, "click", Qt::QueuedConnection);
        // "again": the same button once more after that many ms (e.g. Enabled: TeamSpeak unloads the plugin,
        // then loads it again). The string-based single shot keeps no code of this DLL (unlike a functor).
        const int again = c.value(QString::fromLatin1("again")).toInt(0);
        if (target && again > 0)
            QTimer::singleShot(again, target, SLOT(click()));
        return true;
    }
    if (cmd == QLatin1String("grabwin")) { // a visible top-level window by (part of) its title
        const QString title = c.value(QString::fromLatin1("title")).toString();
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (top->isVisible() && top->windowTitle().contains(title, Qt::CaseInsensitive)) {
                say(QString::fromLatin1("grabwin: \"%1\" -> %2").arg(top->windowTitle(), saveGrab(top, c.value(QString::fromLatin1("name")).toString(QString::fromLatin1("window")))));
                return true;
            }
        }
        say(QString::fromLatin1("grabwin: no window ") + title);
        return true;
    }
    if (cmd == QLatin1String("menus")) {
        if (QWidget* mw = m_chat->mainWindow()) {
            for (QMenu* menu : mw->findChildren<QMenu*>()) {
                if (c.value(QString::fromLatin1("show")).toBool()) // as if it were opened (TeamSpeak may update its items then)
                    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
                QStringList items;
                for (QAction* a : menu->actions()) {
                    if (!a->text().isEmpty())
                        items << a->text() + (a->isEnabled() ? QString() : QString::fromLatin1(" (disabled)"));
                }
                const QString joined = items.join(QString::fromLatin1(" | "));
                if (joined.contains(QLatin1String("TS Media"), Qt::CaseInsensitive) || joined.contains(QLatin1String("Send files"), Qt::CaseInsensitive)
                    || menu->title().contains(QLatin1String("TS Media"), Qt::CaseInsensitive) || menu->title().contains(QLatin1String("Plugins"), Qt::CaseInsensitive))
                    say(QString::fromLatin1("menu \"%1\": %2").arg(menu->title(), joined));
            }
        }
        return true;
    }
    if (cmd == QLatin1String("windows")) {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible())
                say(QString::fromLatin1("window %1 #%2 \"%3\" %4x%5").arg(QString::fromLatin1(w->metaObject()->className()), w->objectName(), w->windowTitle()).arg(w->width()).arg(w->height()));
        }
        return true;
    }
    if (cmd == QLatin1String("setting")) {
        Settings&     s     = Settings::instance();
        const QString key   = c.value(QString::fromLatin1("key")).toString();
        const QJsonValue v  = c.value(QString::fromLatin1("value"));
        if (key == QLatin1String("revealSpoilers"))
            s.revealSpoilers = v.toBool();
        else if (key == QLatin1String("compressVideos"))
            s.compressVideos = v.toBool();
        else if (key == QLatin1String("compressVideosOverMB"))
            s.compressVideosOverMB = v.toInt();
        else if (key == QLatin1String("sendAsAlbum"))
            s.sendAsAlbum = v.toBool();
        else if (key == QLatin1String("autoDownloadMaxMB"))
            s.autoDownloadMaxMB = v.toInt();
        else if (key == QLatin1String("videoAutoDownloadMB"))
            s.videoAutoDownloadMB = v.toInt();
        else if (key == QLatin1String("dataSaver"))
            s.dataSaver = v.toBool();
        else if (key == QLatin1String("uploadMaxMB"))
            s.uploadMaxMB = v.toInt();
        else if (key == QLatin1String("uploadDirectory"))
            s.uploadDirectory = Settings::normalizeUploadDirectory(v.toString()); // Qt-allocated (from the JSON)
        // chat redesign
        else if (key == QLatin1String("chatLayout"))
            s.chatLayout = qBound(Settings::chatLayoutRange.min, v.toInt(), Settings::chatLayoutRange.max);
        else if (key == QLatin1String("chatGroupMessages"))
            s.chatGroupMessages = v.toBool();
        else if (key == QLatin1String("chatHoverActions"))
            s.chatHoverActions = v.toBool();
        else if (key == QLatin1String("chatAvatars"))
            s.chatAvatars = v.toBool();
        else if (key == QLatin1String("chatMentions"))
            s.chatMentions = v.toBool();
        else if (key == QLatin1String("chatCollapseEvents"))
            s.chatCollapseEvents = v.toBool();
        else if (key == QLatin1String("chatLayoutIntroShown"))
            s.chatLayoutIntroShown = v.toBool();
        else if (key == QLatin1String("emojiButton"))
            s.emojiButton = v.toBool();
        else if (key == QLatin1String("hdEmoji"))
            s.hdEmoji = v.toBool();
        else {
            say(QString::fromLatin1("setting: unknown key ") + key);
            return true;
        }
        s.save();
        m_core->onDataSaverChanged();
        m_chat->refreshAll();
        say(QString::fromLatin1("setting: %1 set").arg(key));
        return true;
    }
    if (cmd == QLatin1String("connect")) {
        const QByteArray nick = c.value(QString::fromLatin1("nick")).toString(QString::fromLatin1("TesterB")).toUtf8();
        uint64           tab  = 0;
        unsigned         err  = ts3::funcs.guiConnect
            ? ts3::funcs.guiConnect(PLUGIN_CONNECT_TAB_NEW, nick.constData(), "127.0.0.1:9987", "", nick.constData(), "", "", "", "", "", "", "", "", "", &tab)
            : 1u;
        say(QString::fromLatin1("connect: error %1 tab %2").arg(err).arg(tab));
        return true;
    }
    if (cmd == QLatin1String("say")) {
        const QByteArray text = c.value(QString::fromLatin1("text")).toString().toUtf8();
        const unsigned   err  = ts3::funcs.requestSendChannelTextMsg(sch, text.constData(), ts3::ownChannel(sch), nullptr);
        say(QString::fromLatin1("say: error %1").arg(err));
        return true;
    }
    if (cmd == QLatin1String("quit")) {
        QAction* quit = nullptr;
        if (QWidget* mw = m_chat->mainWindow()) {
            for (QAction* a : mw->findChildren<QAction*>()) {
                if (a->shortcut() == QKeySequence(QString::fromLatin1("Ctrl+Q"))) {
                    quit = a;
                    break;
                }
            }
        }
        say(QString::fromLatin1("quit: action %1").arg(quit ? quit->text() : QString::fromLatin1("not found")));
        if (quit)
            QMetaObject::invokeMethod(quit, "trigger", Qt::QueuedConnection);
        finish(QString::fromLatin1("quit"));
        return false;
    }
    bool       handled = false;
    const bool result  = runPart2(cmd, c, &handled);
    if (handled)
        return result;
    const bool result3 = runPart3(cmd, c, &handled);
    if (handled)
        return result3;
    const bool result4 = runPart4(cmd, c, &handled); // chat redesign
    if (handled)
        return result4;
    say(QString::fromLatin1("unknown command ") + cmd);
    return true;
}

void SelfTest::reactStep(const QString& key, int reaction, int left, int gapMs)
{
    PeerHub* hub = PeerHub::instance();
    if (!hub || !m_chat || !isLocal(ts3::currentConnection())) {
        say(QString::fromLatin1("react: stopped"));
        next(0);
        return;
    }
    const PeerHub::ReactError  error = hub->toggle(key, reaction, m_chat->currentTarget());
    const ReactionView::Entry  entry = hub->view(key).entry(reaction);
    say(QString::fromLatin1("react: %1 reaction %2 -> error %3, count %4 mine %5 (left %6)")
            .arg(key.left(24))
            .arg(reactionName(reaction))
            .arg(static_cast<int>(error))
            .arg(entry.count)
            .arg(entry.mine ? 1 : 0)
            .arg(left - 1));
    if (left <= 1) {
        next(0);
        return;
    }
    auto* t = new QTimer(this);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, this, [this, t, key, reaction, left, gapMs] {
        t->deleteLater();
        reactStep(key, reaction, left - 1, gapMs);
    });
    t->start(gapMs);
}

bool SelfTest::runPart2(const QString& cmd, const QJsonObject& c, bool* handled)
{
    *handled          = true;
    const uint64 sch  = ts3::currentConnection();
    PeerHub*     hub  = PeerHub::instance();
    const auto   name = [&c](const char* key, const char* fallback) { return c.value(QString::fromLatin1(key)).toString(QString::fromLatin1(fallback)); };

    if (cmd == QLatin1String("react")) {
        const QString key = findKey(name("name", ""));
        if (key.isEmpty() || !hub) {
            say(QString::fromLatin1("react: no entry or no hub for ") + name("name", ""));
            return true;
        }
        reactStep(key, reactionOf(c), qMax(1, c.value(QString::fromLatin1("times")).toInt(1)),
                  qMax(0, c.value(QString::fromLatin1("gapMs")).toInt(600)));
        return false;
    }
    if (cmd == QLatin1String("reactmany")) {
        const int     count    = qMax(1, c.value(QString::fromLatin1("count")).toInt(40));
        const int     reaction = reactionOf(c);
        const QString server   = ts3::serverUid(sch);
        const QStringList keys = m_core->keys();
        int done = 0, ok = 0;
        for (int i = keys.size() - 1; i >= 0 && done < count && hub; --i) {
            const MediaEntry* e = m_core->entry(keys.at(i));
            if (!e || e->link.serverUid != server)
                continue;
            ++done;
            if (hub->toggle(keys.at(i), reaction, m_chat->currentTarget()) == PeerHub::ReactError::None)
                ++ok;
        }
        say(QString::fromLatin1("reactmany: %1 toggles, %2 accepted").arg(done).arg(ok));
        return true;
    }
    if (cmd == QLatin1String("reactions")) {
        const QString key = findKey(name("name", ""));
        if (key.isEmpty() || !hub) {
            say(QString::fromLatin1("reactions: no entry for ") + name("name", ""));
            return true;
        }
        const ReactionView view = hub->view(key);
        QStringList        parts;
        for (const ReactionView::Entry& e : view.entries) { // row order
            if (e.count > 0 || e.mine)
                parts << QString::fromLatin1("%1:%2%3[%4]").arg(reactionName(e.reaction)).arg(e.count).arg(e.mine ? QString::fromLatin1("*") : QString()).arg(e.others.join(QLatin1Char(',')));
        }
        say(QString::fromLatin1("reactions: %1 %2 -> %3").arg(m_core->entry(key)->link.fileName, key.left(24), parts.isEmpty() ? QString::fromLatin1("none") : parts.join(QLatin1Char(' '))));
        return true;
    }
    if (cmd == QLatin1String("peers")) {
        if (!hub) {
            say(QString::fromLatin1("peers: no hub"));
            return true;
        }
        const ChatTarget        t = m_chat->currentTarget();
        const peers::PresenceSummary s = hub->directory()->summary(t.sch, t.mode, t.clientId);
        say(QString::fromLatin1("peers: sch %1 own clid %2 channel %3 kind %4 has [%5] without [%6] checking [%7] text \"%8\"")
                .arg(t.sch)
                .arg(ts3::ownClientId(t.sch))
                .arg(ts3::ownChannel(t.sch))
                .arg(static_cast<int>(s.kind))
                .arg(s.has.join(QLatin1Char(',')), s.without.join(QLatin1Char(',')), s.checking.join(QLatin1Char(',')), peers::presenceText(s)));
        for (const QString& line : hub->diagnosticLines())
            say(QString::fromLatin1("peers diag: ") + line);
        return true;
    }
    if (cmd == QLatin1String("move")) { // our own client into another channel of this (localhost) server
        const uint64   cid = static_cast<uint64>(c.value(QString::fromLatin1("cid")).toDouble(1));
        const unsigned err = ts3::funcs.requestClientMove(sch, ts3::ownClientId(sch), cid, "", nullptr);
        say(QString::fromLatin1("move: to channel %1 (error %2)").arg(cid).arg(err));
        next(1500);
        return false;
    }
    if (cmd == QLatin1String("selfvar")) {
        const int var   = c.value(QString::fromLatin1("var")).toInt(-1);
        const int value = c.value(QString::fromLatin1("value")).toInt(0);
        // Only the microphone / speaker flags; never anything else of the user's client.
        if (var != CLIENT_INPUT_MUTED && var != CLIENT_INPUT_DEACTIVATED && var != CLIENT_OUTPUT_MUTED) {
            say(QString::fromLatin1("selfvar: refused %1").arg(var));
            return true;
        }
        const unsigned e1 = ts3::funcs.setClientSelfVariableAsInt(sch, static_cast<size_t>(var), value);
        const unsigned e2 = ts3::funcs.flushClientSelfUpdates(sch, nullptr);
        say(QString::fromLatin1("selfvar: %1 = %2 (error %3 / %4)").arg(var).arg(value).arg(e1).arg(e2));
        return true;
    }
    if (cmd == QLatin1String("vars")) {
        int muted = -1, deact = -1, hw = -1, out = -1;
        const anyID own = ts3::ownClientId(sch);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, &muted);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, &deact);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &hw);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_OUTPUT_MUTED, &out);
        say(QString::fromLatin1("vars: sch %1 clid %2 input_muted %3 input_deactivated %4 input_hardware %5 output_muted %6").arg(sch).arg(own).arg(muted).arg(deact).arg(hw).arg(out));
        return true;
    }
    if (cmd == QLatin1String("command")) {
        const QByteArray text = name("text", "help").toUtf8();
        say(QString::fromLatin1("command: /tsmedia ") + QString::fromUtf8(text));
        ts3plugin_processCommand(sch, text.constData());
        next(800);
        return false;
    }
    if (cmd == QLatin1String("menuitem")) {
        const int     id   = c.value(QString::fromLatin1("id")).toInt(0);
        const QString nick = name("nick", "");
        anyID         clid = 0;
        if (!nick.isEmpty())
            clid = ts3::clientIdByNickname(sch, nick);
        say(QString::fromLatin1("menuitem: %1 on \"%2\" (clid %3)").arg(id).arg(nick).arg(clid));
        ts3plugin_onMenuItemEvent(sch, nick.isEmpty() ? PLUGIN_MENU_TYPE_GLOBAL : PLUGIN_MENU_TYPE_CLIENT, id, clid);
        next(800);
        return false;
    }
    if (cmd == QLatin1String("settings.tab")) {
        QWidget* dialog = m_hooks.settingsDialog ? m_hooks.settingsDialog() : nullptr;
        if (!dialog && m_hooks.openSettings) {
            m_hooks.openSettings();
            dialog = m_hooks.settingsDialog ? m_hooks.settingsDialog() : nullptr;
        }
        auto* tabs = dialog ? dialog->findChild<QTabWidget*>(QString::fromLatin1("tsmediaSettingsTabs")) : nullptr;
        bool  found = false;
        for (int i = 0; tabs && i < tabs->count() && !found; ++i) {
            if (withoutMnemonic(tabs->tabText(i)).contains(name("name", ""), Qt::CaseInsensitive)) {
                tabs->setCurrentIndex(i);
                found = true;
                say(QString::fromLatin1("settings.tab: \"%1\" (%2x%3)").arg(tabs->tabText(i)).arg(dialog->width()).arg(dialog->height()));
            }
        }
        if (!found)
            say(QString::fromLatin1("settings.tab: none named ") + name("name", ""));
        next(1500);
        return false;
    }
    if (cmd == QLatin1String("grabobj")) {
        QWidget* w = topLevelByObject(name("object", "tsmedia"));
        say(QString::fromLatin1("grabobj: %1 -> %2")
                .arg(w ? QString::fromLatin1("%1 \"%2\" %3x%4").arg(w->objectName(), w->windowTitle()).arg(w->width()).arg(w->height()) : QString::fromLatin1("none"),
                     w ? saveGrab(w, name("name", "obj")) : QString()));
        return true;
    }
    if (cmd == QLatin1String("textdump")) {
        QWidget* w = topLevelByObject(name("object", "tsmedia"));
        QString  out;
        if (w) {
            for (QPlainTextEdit* e : w->findChildren<QPlainTextEdit*>())
                out += e->toPlainText() + QString::fromLatin1("\n-----\n");
            for (QTextEdit* e : w->findChildren<QTextEdit*>())
                out += e->toPlainText() + QString::fromLatin1("\n-----\n");
            out += describeWidgetTexts(w);
        }
        writeDebugText(name("name", "textdump"), out);
        say(QString::fromLatin1("textdump: %1 (%2 chars)").arg(w ? w->objectName() : QString::fromLatin1("no window")).arg(out.size()));
        return true;
    }
    if (cmd == QLatin1String("clipboard")) {
        const QString text = QApplication::clipboard() ? QApplication::clipboard()->text() : QString();
        writeDebugText(name("name", "clipboard"), text);
        say(QString::fromLatin1("clipboard: %1 chars").arg(text.size()));
        return true;
    }
    if (cmd == QLatin1String("toast")) {
        QTextBrowser* b     = m_chat->visibleChatBrowser();
        QWidget*      toast = nullptr;
        for (QWidget* w : b ? b->findChildren<QWidget*>(QString::fromLatin1("tsmediaUploadToast")) : QList<QWidget*>()) {
            if (w->isVisible())
                toast = w;
        }
        say(QString::fromLatin1("toast: %1 %2").arg(toast ? QString::fromLatin1("visible") : QString::fromLatin1("none"), toast ? saveGrab(toast, name("name", "toast")) : QString()));
        if (toast)
            say(QString::fromLatin1("toast texts: ") + describeWidgetTexts(toast));
        return true;
    }
    *handled = false;
    return true;
}

// ============================================================================================
// Part 3 of the 2.2 live test: replies and HD emoji. Real Qt events, delivered the way the platform
// would: a right-click (press, release, then a posted context menu event, so TeamSpeak's own menu opens
// from the event loop as it does for a mouse), keys into the chat input (ShortcutOverride, KeyPress,
// KeyRelease), mouse presses on reply lines and on a preview's add-reaction button.
//   msgs {last}                     logs the visible chat's messages as replydoc reads them
//   chatdump {name, last}           writes the last blocks (texts, links, pictures and their properties) to debug/<name>.txt
//   ctxmenu {find, nth, at: text|emoji|nick|preview, media}  right-click on a message (the nth newest whose text
//                                   contains find), on its first HD emoji, or on the newest preview of media
//   menu.log {name}  menu.pick {path:[...]}  menu.close   the open popup menu: logged (and grabbed), an item
//                                   chosen by keyboard (submenus with Right), closed with Esc
//   input.set {text}  input.log     the visible chat input's text; the reply bar's state
//   key {key, mods:[ctrl,alt,shift], text, target: input|popup|focus|<objectName>}   one key press
//   type {text, target}             one key press per character
//   grabwidget {object, up, name}   grabs a visible widget by objectName (prefix), or its up-th parent
//   replyline.click {nth, name}     clicks the nth newest reply line of the visible chat; logs the jump, grabs the flash
//   pm {nick, text}                 a private message to a client of this (localhost) server
//   scrollchat {to: top|bottom|<px>, block, offset}   (block: that block's top, plus offset px, at the view's top)
//   where                           the visible chat's scroll position and the block on top (part 5)
//   react.add {name}                hovers the newest preview named so, then clicks its add-reaction button
//   reactpicker {grab, index, more} the quick reaction picker: grab it, click a reaction, or "+"
//   react.row {name, pill | add}    clicks a pill (its index) or the add pill of the reaction row under a preview
//   clickobj {object}               clicks a visible button by objectName (prefix)
//   hotkey {keyword}                a plugin hotkey (ts3plugin_onHotkeyEvent), e.g. tsmedia_reply
//   quitin {ms}                     TeamSpeak's Quit action ms later; the script goes on meanwhile
//   selectmsg {find, nth, whole}    selects a message's text (whole: with its header) in the visible chat; key target "chat"
//   latemenu {find, delayMs}        a right-click only the plugin sees, then a plain menu like TeamSpeak's delayMs later
//   emojis                          HD emoji counts in the visible chat
//   setemoji {hdEmoji, jumboEmoji, emojiButton}   Settings values, saved and applied
//   input.unfocus                   the focus to the visible chat, as a click there would (an empty input then
//                                   holds TeamSpeak's "Enter Chat Message..."); logs the input 300 ms later
//   input.insert {text, keep}       the plugin's way into the chat input (ChatInputs::insert, as the emoji picker
//                                   and "Use in the chat input" do; keep: as Shift in the picker); logs it, and 700 ms later
//   input.expect {text}             PASS when the input holds exactly text, else FAIL (with the input's state)
//   input.focus {reason}            the focus to the input with reason other|mouse|tab|backtab|active|popup|shortcut;
//                                   logs it at once and 300 ms later (does TeamSpeak take its placeholder out then?)
// 2.2.1 open questions for the live run: TeamSpeak's placeholder out on the plugin's focus (reason other), its
// colour, and Ctrl+Z after an emoji never bringing it back:
//   {"cmd":"input.set","text":""} {"cmd":"input.unfocus"} {"cmd":"input.focus","reason":"other"} {"cmd":"input.log"}
//   (the same with "mouse" and "popup"), then {"cmd":"input.unfocus"} {"cmd":"input.insert","text":"\ud83d\ude00"}
//   {"cmd":"key","key":"Z","mods":["ctrl"]} {"cmd":"key","key":"Z","mods":["ctrl"]} {"cmd":"input.log"}  -> empty, no placeholder
// 2.2.1 placeholder check (an emoji went out as "<emoji>Enter Chat Message..."):
//   {"cmd":"input.set","text":""} {"cmd":"input.unfocus"} {"cmd":"input.log"}   -> the placeholder shown and learned
//   {"cmd":"clickobj","object":"tsmediaEmojiButton"} {"cmd":"type","text":"rocket","target":"popup"}
//   {"cmd":"key","key":"Return","target":"popup"} {"cmd":"input.expect","text":"\ud83d\ude80"}
//   {"cmd":"input.set","text":""} {"cmd":"input.unfocus"} {"cmd":"ctxmenu","at":"emoji"}
//   {"cmd":"menu.pick","path":["&Use in the chat input"]} {"cmd":"input.expect","text":"<that emoji>"}
//   {"cmd":"input.set","text":""} {"cmd":"input.unfocus"} {"cmd":"input.insert","text":"\ud83d\ude00"} {"cmd":"input.expect","text":"\ud83d\ude00"}
// ============================================================================================

void SelfTest::later(int ms, std::function<void()> fn)
{
    auto* t = new QTimer(this); // a child: stops with the driver (CRASH RULE)
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, this, [t, fn = std::move(fn)] {
        t->deleteLater();
        fn();
    });
    t->start(qMax(0, ms));
}

namespace {

int keyCode(const QString& name)
{
    static const QHash<QString, int> keys = {
        {QString::fromLatin1("Return"), Qt::Key_Return},     {QString::fromLatin1("Enter"), Qt::Key_Enter},
        {QString::fromLatin1("Escape"), Qt::Key_Escape},     {QString::fromLatin1("Up"), Qt::Key_Up},
        {QString::fromLatin1("Down"), Qt::Key_Down},         {QString::fromLatin1("Left"), Qt::Key_Left},
        {QString::fromLatin1("Right"), Qt::Key_Right},       {QString::fromLatin1("Tab"), Qt::Key_Tab},
        {QString::fromLatin1("Space"), Qt::Key_Space},       {QString::fromLatin1("Backspace"), Qt::Key_Backspace},
        {QString::fromLatin1("Home"), Qt::Key_Home},         {QString::fromLatin1("End"), Qt::Key_End},
        {QString::fromLatin1("PageUp"), Qt::Key_PageUp},     {QString::fromLatin1("PageDown"), Qt::Key_PageDown},
    };
    if (keys.contains(name))
        return keys.value(name);
    if (name.size() == 1 && name.at(0).isLetterOrNumber())
        return name.at(0).toUpper().unicode();
    return 0;
}

void sendKey(QWidget* target, int key, Qt::KeyboardModifiers mods, const QString& text)
{
    const quint32 vk = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ? static_cast<quint32>(key) : 0u;
    QKeyEvent     over(QEvent::ShortcutOverride, key, mods, 0, vk, 0, text);
    QApplication::sendEvent(target, &over);
    QKeyEvent press(QEvent::KeyPress, key, mods, 0, vk, 0, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, 0, vk, 0, text);
    QApplication::sendEvent(target, &release);
}

void sendMouse(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(pos), QPointF(w->mapTo(w->window(), pos)), QPointF(w->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

QTextCharFormat charFormatAt(QTextDocument* doc, int position)
{
    QTextCursor cursor(doc);
    cursor.setPosition(position);
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

// Scrolls b so that block shows (if it doesn't).
void ensureShown(QTextBrowser* b, const QTextBlock& block)
{
    const QRectF r   = b->document()->documentLayout()->blockBoundingRect(block);
    QScrollBar*  bar = b->verticalScrollBar();
    const int    h   = b->viewport()->height();
    if (r.top() < bar->value() || r.bottom() > bar->value() + h)
        bar->setValue(qBound(bar->minimum(), qRound(r.top()) - 40, bar->maximum()));
}

// The viewport rect of the character at position (its line's height).
QRect charRect(QTextBrowser* b, int position)
{
    QTextDocument*     doc    = b->document();
    const QTextBlock   tb     = doc->findBlock(position);
    const QTextLayout* layout = tb.layout();
    if (!layout)
        return {};
    const int       rel  = position - tb.position();
    const QTextLine line = layout->lineForTextPosition(rel);
    if (!line.isValid())
        return {};
    const QRectF br = doc->documentLayout()->blockBoundingRect(tb);
    const qreal  x1 = line.cursorToX(rel);
    const qreal  x2 = line.cursorToX(rel + 1);
    return QRectF(br.left() + qMin(x1, x2) - b->horizontalScrollBar()->value(), br.top() + line.y() - b->verticalScrollBar()->value(), qMax(1.0, qAbs(x2 - x1)),
                  line.height())
        .toAlignedRect();
}

QString visible(const QString& text)
{
    QString out;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u < 0x20 || u == 0x2028 || u == 0x2029 || u == 0x200b || u == 0x2060 || u == 0xfffc || (u >= 0x200e && u <= 0x200f) || (u >= 0x202a && u <= 0x202e))
            out += QString::fromLatin1("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
        else
            out += ch;
    }
    return out;
}

QString describeFormat(const QTextCharFormat& f)
{
    QStringList parts;
    if (f.isAnchor())
        parts << QString::fromLatin1("href=") + f.anchorHref().left(160);
    if (f.isImageFormat()) {
        const QTextImageFormat img = f.toImageFormat();
        parts << QString::fromLatin1("img=%1 %2x%3").arg(img.name().left(80)).arg(img.width()).arg(img.height());
        if (emojiformat::isHd(f)) {
            parts << QString::fromLatin1("hd orig=\"%1\" emoticon=%2 jumbo=%3")
                         .arg(visible(emojiformat::originalText(f)), emojiformat::emoticonName(f))
                         .arg(f.boolProperty(emojiformat::kJumbo) ? 1 : 0);
        }
        if (f.hasProperty(replydoc::kObjectProperty)) {
            QStringList runs;
            const QVariantList list = f.property(replydoc::kRunsProperty).toList();
            for (int i = 0; i + 1 < list.size(); i += 2) {
                const QTextCharFormat rf = qvariant_cast<QTextFormat>(list.at(i + 1)).toCharFormat();
                QString           run  = QLatin1Char('"') + visible(list.at(i).toString()) + QLatin1Char('"');
                if (rf.isAnchor())
                    run += QString::fromLatin1("{href=") + rf.anchorHref().left(120) + QLatin1Char('}');
                if (rf.isImageFormat())
                    run += QString::fromLatin1("{img=") + rf.toImageFormat().name() + (emojiformat::isHd(rf) ? QString::fromLatin1(" hd") : QString()) + QLatin1Char('}');
                runs << run;
            }
            parts << QString::fromLatin1("REPLYLINE runs=[") + runs.join(QLatin1Char(' ')) + QLatin1Char(']');
        }
    }
    // Chat redesign: our pictures and texts (the TeamSpeak runs they keep), and what we recoloured.
    if (layoutformat::isOurs(f)) {
        QStringList        runs;
        const QVariantList list = layoutformat::runsOf(f);
        for (int i = 0; i + 1 < list.size(); i += 2) {
            const QTextCharFormat rf = qvariant_cast<QTextFormat>(list.at(i + 1)).toCharFormat();
            QString           run  = QLatin1Char('"') + visible(list.at(i).toString()) + QLatin1Char('"');
            if (rf.isAnchor())
                run += QString::fromLatin1("{href=") + rf.anchorHref().left(120) + QLatin1Char('}');
            if (rf.isImageFormat())
                run += QString::fromLatin1("{img=") + rf.toImageFormat().name() + QLatin1Char('}');
            if (rf.foreground().style() != Qt::NoBrush)
                run += QString::fromLatin1("{fg=") + rf.foreground().color().name() + QLatin1Char('}');
            runs << run;
        }
        parts << QString::fromLatin1("LAYOUT kind=%1 time=%2 runs=[%3]").arg(static_cast<int>(layoutformat::kindOf(f))).arg(f.stringProperty(layoutformat::kTime), runs.join(QLatin1Char(' ')));
    }
    if (f.hasProperty(layoutformat::kApplied))
        parts << QString::fromLatin1("APPLIED");
    if (f.foreground().style() != Qt::NoBrush)
        parts << QString::fromLatin1("fg=") + f.foreground().color().name();
    if (f.fontItalic())
        parts << QString::fromLatin1("i");
    if (f.fontWeight() > QFont::Normal)
        parts << QString::fromLatin1("w=%1").arg(f.fontWeight());
    return parts.join(QLatin1Char(' '));
}

// Chat redesign: a block format as the layout sets it (TeamSpeak's own: "-").
QString describeBlock(const QTextBlockFormat& f)
{
    QStringList parts;
    if (f.hasProperty(layoutformat::kKind))
        parts << QString::fromLatin1("row=%1 tag=%2").arg(f.intProperty(layoutformat::kKind)).arg(f.intProperty(layoutformat::kBlockTag));
    if (f.hasProperty(QTextFormat::BlockLeftMargin) || f.hasProperty(QTextFormat::TextIndent))
        parts << QString::fromLatin1("left=%1 indent=%2").arg(f.leftMargin()).arg(f.textIndent());
    if (f.hasProperty(QTextFormat::BlockTopMargin))
        parts << QString::fromLatin1("top=%1").arg(f.topMargin());
    if (f.hasProperty(QTextFormat::LineHeightType))
        parts << QString::fromLatin1("line=%1/%2").arg(f.lineHeight()).arg(f.lineHeightType());
    return parts.isEmpty() ? QString::fromLatin1("-") : parts.join(QLatin1Char(' '));
}

} // namespace

bool SelfTest::runPart3(const QString& cmd, const QJsonObject& c, bool* handled)
{
    *handled          = true;
    const uint64 sch  = ts3::currentConnection();
    const auto   str  = [&c](const char* key, const char* fallback) { return c.value(QString::fromLatin1(key)).toString(QString::fromLatin1(fallback)); };
    QTextBrowser* b   = m_chat->visibleChatBrowser();
    const auto input  = [this]() -> QTextEdit* {
        for (const QPointer<QWidget>& w : m_chat->m_inputs) {
            if (w && w->isVisible()) {
                if (auto* e = qobject_cast<QTextEdit*>(w.data()))
                    return e;
            }
        }
        return nullptr;
    };
    const auto widgetNamed = [](const QString& prefix) -> QWidget* {
        for (QWidget* w : QApplication::allWidgets()) {
            if (w->isVisible() && w->objectName().startsWith(prefix))
                return w;
        }
        return nullptr;
    };
    // 2.2.1: the input's text, focus and what ChatInputs knows of TeamSpeak's placeholder in it.
    const QPointer<ChatIntegration> integration = m_chat;
    const auto inputState = [integration](QTextEdit* e) -> QString {
        if (!e)
            return QString::fromLatin1("no input");
        ChatInputs* inputs = integration && integration->m_emoji ? integration->m_emoji->inputs() : nullptr;
        return QString::fromLatin1("\"%1\" focus %2 placeholder \"%3\" shown %4 pending %5")
            .arg(visible(e->toPlainText()))
            .arg(e->hasFocus() ? 1 : 0)
            .arg(inputs ? visible(inputs->placeholder(e)) : QString::fromLatin1("?"))
            .arg(inputs && inputs->showsPlaceholder(e) ? 1 : 0)
            .arg(inputs && inputs->hasPending(e) ? 1 : 0);
    };
    const auto targetOf = [&](const QString& t) -> QWidget* {
        if (t.isEmpty() || t == QLatin1String("input"))
            return input();
        if (t == QLatin1String("popup")) {
            QWidget* p = QApplication::activePopupWidget();
            return p ? (p->focusWidget() ? p->focusWidget() : p) : nullptr;
        }
        if (t == QLatin1String("focus"))
            return QApplication::focusWidget();
        if (t == QLatin1String("chat"))
            return b;
        return widgetNamed(t);
    };
    const auto modsOf = [&c]() {
        Qt::KeyboardModifiers mods = Qt::NoModifier;
        for (const QJsonValue& v : c.value(QString::fromLatin1("mods")).toArray()) {
            const QString m = v.toString();
            if (m == QLatin1String("ctrl"))
                mods |= Qt::ControlModifier;
            else if (m == QLatin1String("alt"))
                mods |= Qt::AltModifier;
            else if (m == QLatin1String("shift"))
                mods |= Qt::ShiftModifier;
        }
        return mods;
    };
    // The nth newest message block of b whose text (or file label) contains find.
    const auto findMessage = [&c, &str](QTextBrowser* browser) -> QTextBlock {
        const QString find = str("find", "");
        int           nth  = c.value(QString::fromLatin1("nth")).toInt(0);
        for (QTextBlock tb = browser->document()->lastBlock(); tb.isValid(); tb = tb.previous()) {
            if (!tb.isVisible())
                continue;
            const replydoc::Message m = replydoc::parseBlock(tb);
            if (m.block < 0)
                continue;
            if (!find.isEmpty() && !(m.text + QLatin1Char(' ') + m.mediaLabel).contains(find, Qt::CaseInsensitive))
                continue;
            if (nth-- > 0)
                continue;
            return tb;
        }
        return QTextBlock();
    };

    if (cmd == QLatin1String("msgs")) {
        if (!b) {
            say(QString::fromLatin1("msgs: no visible chat"));
            return true;
        }
        const QVector<replydoc::Message> ms   = replydoc::scan(b->document());
        const int                        last = c.value(QString::fromLatin1("last")).toInt(12);
        say(QString::fromLatin1("msgs: %1 messages, %2 blocks, scroll %3/%4").arg(ms.size()).arg(b->document()->blockCount()).arg(b->verticalScrollBar()->value()).arg(b->verticalScrollBar()->maximum()));
        for (int i = qMax(0, ms.size() - last); i < ms.size(); ++i) {
            const replydoc::Message& m = ms.at(i);
            say(QString::fromLatin1("msg %1 block %2 nick \"%3\" uid %4 time %5 text \"%6\" media \"%7\" quote %8 restyled %9")
                    .arg(i)
                    .arg(m.block)
                    .arg(m.nick, m.uid.isEmpty() ? QString::fromLatin1("-") : QString::fromLatin1("yes"))
                    .arg(m.minutes)
                    .arg(visible(m.text.left(80)), m.mediaLabel.left(60))
                    .arg(m.hasQuote ? QString::fromLatin1("{%1 @%2 \"%3\"%4}").arg(m.quote.nick).arg(m.quote.minutes).arg(visible(m.quote.snippet.left(70)), m.quote.media ? QString::fromLatin1(" media") : QString())
                                    : QString::fromLatin1("-"))
                    .arg(m.restyled ? 1 : 0));
        }
        return true;
    }
    if (cmd == QLatin1String("chatdump")) {
        if (!b) {
            say(QString::fromLatin1("chatdump: no visible chat"));
            return true;
        }
        QTextDocument* doc  = b->document();
        const int      last = c.value(QString::fromLatin1("last")).toInt(10);
        // Chat redesign: the undo stack (never changed by us) and each block's format.
        QString out = QString::fromLatin1("undo/redo enabled %1, %2 blocks\n").arg(doc->isUndoRedoEnabled() ? 1 : 0).arg(doc->blockCount());
        for (QTextBlock tb = doc->findBlockByNumber(qMax(0, doc->blockCount() - last)); tb.isValid(); tb = tb.next()) {
            out += QString::fromLatin1("== block %1 visible %2 pos %3 len %4 format %5\n")
                       .arg(tb.blockNumber())
                       .arg(tb.isVisible() ? 1 : 0)
                       .arg(tb.position())
                       .arg(tb.length())
                       .arg(describeBlock(tb.blockFormat()));
            for (auto it = tb.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (!f.isValid())
                    continue;
                out += QString::fromLatin1("  [%1] \"%2\" %3\n").arg(f.position()).arg(visible(f.text().left(400)), describeFormat(f.charFormat()));
            }
        }
        writeDebugText(str("name", "chatdump"), out);
        say(QString::fromLatin1("chatdump: %1 chars").arg(out.size()));
        return true;
    }
    if (cmd == QLatin1String("ctxmenu")) {
        if (!b) {
            say(QString::fromLatin1("ctxmenu: no visible chat"));
            return true;
        }
        const QString at = str("at", "text");
        QPoint        pos(-1, -1);
        QString       what;
        if (at == QLatin1String("preview")) {
            const QString key  = findKey(str("media", ""));
            auto*         view = m_chat->viewFor(b);
            if (key.isEmpty() || !view) {
                say(QString::fromLatin1("ctxmenu: no entry for ") + str("media", ""));
                return true;
            }
            m_chat->ensurePositions(*view);
            const QVector<int> positions = view->positionsByKey.value(key);
            if (positions.isEmpty()) {
                say(QString::fromLatin1("ctxmenu: no preview of ") + str("media", ""));
                return true;
            }
            ensureShown(b, b->document()->findBlock(positions.first()));
            m_chat->ensurePositions(*view);
            QRectF r = m_chat->previewRect(b, positions.first(), QSizeF(view->formatSizes.value(key)));
            if (m_chat->m_reactions)
                r = m_chat->m_reactions->pictureRect(b, key, r);
            pos  = r.center().toPoint();
            what = QString::fromLatin1("preview of ") + m_core->entry(key)->link.fileName;
        } else {
            const QTextBlock tb = findMessage(b);
            if (!tb.isValid()) {
                say(QString::fromLatin1("ctxmenu: no message with \"%1\"").arg(str("find", "")));
                return true;
            }
            ensureShown(b, tb);
            const replydoc::Message m = replydoc::parseBlock(tb);
            int                     p = -1;
            if (at == QLatin1String("emoji")) {
                for (int q = m.textStart; q < tb.position() + tb.length() - 1 && p < 0; ++q) {
                    if (emojiformat::isHd(charFormatAt(b->document(), q)))
                        p = q;
                }
            } else if (at == QLatin1String("nick")) { // the header's client link (TeamSpeak's own client menu)
                for (int q = tb.position(); q < m.textStart && p < 0; ++q) {
                    const QTextCharFormat f = charFormatAt(b->document(), q);
                    if (f.isAnchor() && f.anchorHref().startsWith(QLatin1String("client://")))
                        p = layoutformat::isOurs(f) ? q : q + 2; // chat redesign: the head picture is that link
                }
            } else {
                p = qMin(m.textStart + 1, tb.position() + tb.length() - 2);
            }
            if (p < 0) {
                say(QString::fromLatin1("ctxmenu: nothing to click (%1) in block %2").arg(at).arg(tb.blockNumber()));
                return true;
            }
            const QRect r = charRect(b, p);
            pos           = r.center();
            what          = QString::fromLatin1("%1 of block %2 (\"%3\")").arg(at).arg(tb.blockNumber()).arg(visible(m.text.left(40)));
        }
        QWidget*     vp     = b->viewport();
        const QPoint global = vp->mapToGlobal(pos);
        say(QString::fromLatin1("ctxmenu: %1 at %2,%3").arg(what).arg(pos.x()).arg(pos.y()));
        sendMouse(vp, QEvent::MouseButtonPress, pos, Qt::RightButton, Qt::RightButton);
        sendMouse(vp, QEvent::MouseButtonRelease, pos, Qt::RightButton, Qt::NoButton);
        QCoreApplication::postEvent(vp, new QContextMenuEvent(QContextMenuEvent::Mouse, pos, global, Qt::NoModifier));
        next(c.value(QString::fromLatin1("ms")).toInt(900)); // the menu opens from the event loop (TeamSpeak's may run its own loop)
        return false;
    }
    if (cmd == QLatin1String("latemenu")) {
        // A chat menu that TeamSpeak builds late (seen once on a nickname: TeamSpeak's client menu came after the
        // plugin's own small menu). The right-click goes to the plugin's filter only; TeamSpeak never sees it, and
        // a plain menu like TeamSpeak's pops up delayMs later at the same place.
        const QTextBlock tb = b ? findMessage(b) : QTextBlock();
        if (!tb.isValid()) {
            say(QString::fromLatin1("latemenu: no message with \"%1\"").arg(str("find", "")));
            return true;
        }
        ensureShown(b, tb);
        const replydoc::Message m      = replydoc::parseBlock(tb);
        const QPoint            pos    = charRect(b, qMin(m.textStart + 1, tb.position() + tb.length() - 2)).center();
        QWidget*                vp     = b->viewport();
        const QPoint            global = vp->mapToGlobal(pos);
        QContextMenuEvent       event(QContextMenuEvent::Mouse, pos, global, Qt::NoModifier);
        m_chat->eventFilter(vp, &event);
        const int delay = c.value(QString::fromLatin1("delayMs")).toInt(300);
        say(QString::fromLatin1("latemenu: block %1, a plain menu in %2 ms").arg(tb.blockNumber()).arg(delay));
        const QPointer<QWidget> guard(vp);
        later(delay, [this, guard, global] {
            QWidget* popup = QApplication::activePopupWidget();
            say(QString::fromLatin1("latemenu: before ours, popup %1").arg(popup ? popup->objectName() + QLatin1Char(' ') + QString::fromLatin1(popup->metaObject()->className()) : QString::fromLatin1("none")));
            if (guard) {
                auto* menu = new QMenu(guard.data()); // like TeamSpeak's: no tsmedia name
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->addAction(QString::fromLatin1("Late item 1"));
                menu->addAction(QString::fromLatin1("Late item 2"));
                menu->popup(global);
            }
            next(500);
        });
        return false;
    }
    if (cmd == QLatin1String("menu.log")) {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            QWidget* popup = QApplication::activePopupWidget();
            say(QString::fromLatin1("menu.log: no menu open (popup %1)").arg(popup ? popup->objectName() + QLatin1Char(' ') + QString::fromLatin1(popup->metaObject()->className()) : QString::fromLatin1("none")));
            return true;
        }
        QStringList items;
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) {
                items << QString::fromLatin1("---");
                continue;
            }
            items << a->text() + (a->menu() ? QString::fromLatin1(" >") : QString()) + (a->isEnabled() ? QString() : QString::fromLatin1(" (disabled)"))
                         + (a->isCheckable() ? (a->isChecked() ? QString::fromLatin1(" [x]") : QString::fromLatin1(" [ ]")) : QString())
                         + (a->icon().isNull() ? QString() : QString::fromLatin1(" {icon}"));
        }
        say(QString::fromLatin1("menu.log: %1#%2 %3x%4 at %5,%6: %7")
                .arg(QString::fromLatin1(menu->metaObject()->className()), menu->objectName())
                .arg(menu->width())
                .arg(menu->height())
                .arg(menu->x())
                .arg(menu->y())
                .arg(items.join(QString::fromLatin1(" | "))));
        for (QAction* a : menu->actions()) { // submenus, one level
            if (!a->menu())
                continue;
            QStringList sub;
            for (QAction* s : a->menu()->actions())
                sub << (s->isSeparator() ? QString::fromLatin1("---") : s->text());
            say(QString::fromLatin1("menu.log:   > \"%1\": %2").arg(a->text(), sub.join(QString::fromLatin1(" | "))));
        }
        if (!str("name", "").isEmpty())
            say(QString::fromLatin1("menu.log: -> ") + saveGrab(menu, str("name", "")));
        return true;
    }
    if (cmd == QLatin1String("menu.pick")) {
        auto*             menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        const QJsonArray  path = c.value(QString::fromLatin1("path")).toArray();
        for (int i = 0; menu && i < path.size(); ++i) {
            const QString want   = path.at(i).toString();
            QAction*      chosen = nullptr;
            for (QAction* a : menu->actions()) {
                if (!a->isSeparator() && withoutMnemonic(a->text()).startsWith(want, Qt::CaseInsensitive)) {
                    chosen = a;
                    break;
                }
            }
            if (!chosen) {
                say(QString::fromLatin1("menu.pick: no item \"%1\"").arg(want));
                menu = nullptr;
                break;
            }
            menu->setActiveAction(chosen);
            if (i + 1 < path.size() && chosen->menu()) {
                sendKey(menu, Qt::Key_Right, Qt::NoModifier, QString());
                say(QString::fromLatin1("menu.pick: opened \"%1\" (%2)").arg(chosen->text()).arg(chosen->menu()->isVisible() ? 1 : 0));
                menu = chosen->menu();
                continue;
            }
            say(QString::fromLatin1("menu.pick: \"%1\" enabled %2").arg(chosen->text()).arg(chosen->isEnabled() ? 1 : 0));
            sendKey(menu, Qt::Key_Return, Qt::NoModifier, QString());
        }
        next(700);
        return false;
    }
    if (cmd == QLatin1String("menu.close")) {
        int closed = 0;
        while (QWidget* popup = QApplication::activePopupWidget()) {
            if (++closed > 5)
                break;
            popup->close();
        }
        say(QString::fromLatin1("menu.close: %1 popup(s)").arg(closed));
        next(400);
        return false;
    }
    if (cmd == QLatin1String("input.set")) {
        QTextEdit* e = input();
        if (!e) {
            say(QString::fromLatin1("input.set: no chat input"));
            return true;
        }
        e->setFocus(Qt::OtherFocusReason); // first: TeamSpeak takes its placeholder out itself (2.2.1)
        e->clear();
        QTextCursor cursor = e->textCursor();
        cursor.insertText(c.value(QString::fromLatin1("text")).toString());
        e->setTextCursor(cursor);
        say(QString::fromLatin1("input.set: \"%1\"").arg(visible(e->toPlainText())));
        return true;
    }
    if (cmd == QLatin1String("input.log")) {
        QTextEdit* e   = input();
        QWidget*   bar = widgetNamed(QString::fromLatin1("tsmediaReplyBar"));
        say(QString::fromLatin1("input.log: \"%1\" (%2) bar %3%4")
                .arg(e ? visible(e->toPlainText()) : QString::fromLatin1("no input"), e ? QString::fromLatin1(e->metaObject()->className()) : QString())
                .arg(bar ? QString::fromLatin1("shown %1x%2").arg(bar->width()).arg(bar->height()) : QString::fromLatin1("hidden"))
                .arg(bar ? QString::fromLatin1(" ") + describeWidgetTexts(bar) : QString()));
        if (e) { // 2.2.1: the placeholder state, and the text's own colour (TeamSpeak's placeholder grey)
            QTextCursor first(e->document());
            first.setPosition(e->document()->isEmpty() ? 0 : 1);
            say(QString::fromLatin1("input.log: state %1, first character %2").arg(inputState(e), describeFormat(first.charFormat())));
        }
        return true;
    }
    if (cmd == QLatin1String("input.unfocus")) { // 2.2.1
        const QPointer<QTextEdit> e(input());
        if (b)
            b->setFocus(Qt::MouseFocusReason);
        say(QString::fromLatin1("input.unfocus: the focus to %1").arg(b ? QString::fromLatin1("the chat") : QString::fromLatin1("nothing (no visible chat)")));
        later(300, [this, e, inputState] {
            say(QString::fromLatin1("input.unfocus: %1").arg(inputState(e.data())));
            next();
        });
        return false;
    }
    if (cmd == QLatin1String("input.focus")) { // 2.2.1 review: TeamSpeak's own handling, per focus reason
        const QPointer<QTextEdit> e(input());
        if (!e) {
            say(QString::fromLatin1("input.focus: no chat input"));
            return true;
        }
        static const struct {
            const char*     name;
            Qt::FocusReason reason;
        } reasons[] = {{"other", Qt::OtherFocusReason},   {"mouse", Qt::MouseFocusReason},          {"tab", Qt::TabFocusReason},
                       {"backtab", Qt::BacktabFocusReason}, {"active", Qt::ActiveWindowFocusReason}, {"popup", Qt::PopupFocusReason},
                       {"shortcut", Qt::ShortcutFocusReason}};
        const QString   name   = str("reason", "other");
        Qt::FocusReason reason = Qt::OtherFocusReason;
        for (const auto& r : reasons) {
            if (name == QLatin1String(r.name))
                reason = r.reason;
        }
        say(QString::fromLatin1("input.focus: reason %1 (%2), before %3").arg(name).arg(static_cast<int>(reason)).arg(inputState(e.data())));
        if (!e->window()->isActiveWindow())
            e->window()->activateWindow();
        e->setFocus(reason);
        say(QString::fromLatin1("input.focus: now %1").arg(inputState(e.data())));
        later(300, [this, e, inputState] {
            say(QString::fromLatin1("input.focus: 300 ms later %1").arg(inputState(e.data())));
            next();
        });
        return false;
    }
    if (cmd == QLatin1String("input.insert")) { // 2.2.1
        const QPointer<QTextEdit> e(input());
        ChatInputs*               inputs = m_chat->m_emoji ? m_chat->m_emoji->inputs() : nullptr;
        if (!e || !inputs) {
            say(QString::fromLatin1("input.insert: no chat input"));
            return true;
        }
        const QString text = str("text", "");
        say(QString::fromLatin1("input.insert: \"%1\" into %2").arg(visible(text), inputState(e.data())));
        inputs->insert(e.data(), text, c.value(QString::fromLatin1("keep")).toBool());
        say(QString::fromLatin1("input.insert: now %1").arg(inputState(e.data())));
        later(700, [this, e, inputState] {
            say(QString::fromLatin1("input.insert: 700 ms later %1").arg(inputState(e.data())));
            next();
        });
        return false;
    }
    if (cmd == QLatin1String("input.expect")) { // 2.2.1
        QTextEdit*    e    = input();
        const QString want = str("text", "");
        const bool    ok   = e && e->toPlainText() == want;
        say(QString::fromLatin1("input.expect: %1 (want \"%2\", have %3)").arg(ok ? QString::fromLatin1("PASS") : QString::fromLatin1("FAIL"), visible(want), inputState(e)));
        return true;
    }
    if (cmd == QLatin1String("key") || cmd == QLatin1String("type")) {
        QWidget* target = targetOf(str("target", "input"));
        if (!target) {
            say(cmd + QString::fromLatin1(": no target ") + str("target", "input"));
            return true;
        }
        if (cmd == QLatin1String("type")) {
            const QString text = str("text", "");
            for (const QChar ch : text)
                sendKey(target, ch.isLetterOrNumber() ? ch.toUpper().unicode() : 0, Qt::NoModifier, QString(ch));
            say(QString::fromLatin1("type: \"%1\" into %2#%3").arg(text, QString::fromLatin1(target->metaObject()->className()), target->objectName()));
        } else {
            const int key = keyCode(str("key", ""));
            sendKey(target, key, modsOf(), str("text", ""));
            say(QString::fromLatin1("key: %1 (mods %2) into %3#%4").arg(str("key", "")).arg(static_cast<int>(modsOf())).arg(QString::fromLatin1(target->metaObject()->className()), target->objectName()));
        }
        next(c.value(QString::fromLatin1("ms")).toInt(500));
        return false;
    }
    if (cmd == QLatin1String("grabwidget")) {
        QWidget* w = widgetNamed(str("object", "tsmedia"));
        for (int i = 0; w && i < c.value(QString::fromLatin1("up")).toInt(0) && w->parentWidget(); ++i)
            w = w->parentWidget();
        say(QString::fromLatin1("grabwidget: %1 -> %2")
                .arg(w ? QString::fromLatin1("%1#%2 %3x%4").arg(QString::fromLatin1(w->metaObject()->className()), w->objectName()).arg(w->width()).arg(w->height()) : QString::fromLatin1("none"),
                     w ? saveGrab(w, str("name", "widget")) : QString()));
        return true;
    }
    if (cmd == QLatin1String("replyline.click")) {
        if (!b) {
            say(QString::fromLatin1("replyline.click: no visible chat"));
            return true;
        }
        const QVector<int> objects = replydoc::objectPositions(b->document());
        const int          nth     = c.value(QString::fromLatin1("nth")).toInt(0);
        if (nth >= objects.size()) {
            say(QString::fromLatin1("replyline.click: %1 reply lines").arg(objects.size()));
            return true;
        }
        const int p = objects.at(objects.size() - 1 - nth);
        ensureShown(b, b->document()->findBlock(p));
        const QTextImageFormat img = charFormatAt(b->document(), p).toImageFormat();
        QRect                  r   = charRect(b, p);
        r.setWidth(qRound(img.width()));
        const QPoint pos    = QPoint(r.left() + qMin(60, r.width() / 2), r.center().y());
        QScrollBar*  bar    = b->verticalScrollBar();
        const int    before = bar->value();
        QWidget*     vp     = b->viewport();
        sendMouse(vp, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
        say(QString::fromLatin1("replyline.click: line at %1 (block %2) clicked at %3,%4; scroll %5")
                .arg(p)
                .arg(b->document()->findBlock(p).blockNumber())
                .arg(pos.x())
                .arg(pos.y())
                .arg(before));
        const QPointer<QTextBrowser> guard(b);
        const QString                name = str("name", "");
        later(c.value(QString::fromLatin1("grabMs")).toInt(650), [this, guard, name, widgetNamed] {
            QWidget* flash = widgetNamed(QString::fromLatin1("tsmediaReplyFlash"));
            say(QString::fromLatin1("replyline.click: flash %1, scroll %2")
                    .arg(flash ? QString::fromLatin1("shown at y %1 h %2").arg(flash->y()).arg(flash->height()) : QString::fromLatin1("none"))
                    .arg(guard ? guard->verticalScrollBar()->value() : -1));
            if (guard && !name.isEmpty())
                say(QString::fromLatin1("replyline.click: -> ") + saveGrab(guard.data(), name));
        });
        later(2200, [this, guard, widgetNamed] {
            say(QString::fromLatin1("replyline.click: after 2.2 s flash %1, scroll %2")
                    .arg(widgetNamed(QString::fromLatin1("tsmediaReplyFlash")) ? QString::fromLatin1("still shown") : QString::fromLatin1("gone"))
                    .arg(guard ? guard->verticalScrollBar()->value() : -1));
            next(0);
        });
        return false;
    }
    if (cmd == QLatin1String("pm")) {
        const anyID      clid = ts3::clientIdByNickname(sch, str("nick", ""));
        const QByteArray text = c.value(QString::fromLatin1("text")).toString().toUtf8();
        const unsigned   err  = clid ? ts3::funcs.requestSendPrivateTextMsg(sch, text.constData(), clid, nullptr) : 1u;
        say(QString::fromLatin1("pm: to %1 (clid %2) error %3").arg(str("nick", "")).arg(clid).arg(err));
        next(800);
        return false;
    }
    if (cmd == QLatin1String("scrollchat")) {
        if (b) {
            QScrollBar*   bar = b->verticalScrollBar();
            const QString to  = str("to", "bottom");
            if (c.contains(QString::fromLatin1("block"))) { // part 5: that block's top (plus offset px) at the top of the view
                const QTextBlock tb = b->document()->findBlockByNumber(c.value(QString::fromLatin1("block")).toInt());
                if (tb.isValid())
                    bar->setValue(qRound(b->document()->documentLayout()->blockBoundingRect(tb).top()) + c.value(QString::fromLatin1("offset")).toInt(0));
            } else {
                bar->setValue(to == QLatin1String("top") ? bar->minimum() : to == QLatin1String("bottom") ? bar->maximum() : to.toInt());
            }
            say(QString::fromLatin1("scrollchat: %1/%2").arg(bar->value()).arg(bar->maximum()));
        }
        next(400);
        return false;
    }
    if (cmd == QLatin1String("where")) { // part 5: the reader's place: the scroll bar and the block on top
        if (!b) {
            say(QString::fromLatin1("where: no visible chat"));
            return true;
        }
        QScrollBar*             bar = b->verticalScrollBar();
        const layoutdoc::Anchor a   = layoutdoc::anchorAt(b->document(), bar->value());
        const QTextBlock        tb  = b->document()->findBlockByNumber(a.block);
        say(QString::fromLatin1("where: scroll %1/%2 (at bottom %3), top block %4 visible %5 offset %6 of %7: \"%8\"")
                .arg(bar->value())
                .arg(bar->maximum())
                .arg(bar->value() >= bar->maximum() - 4 ? 1 : 0)
                .arg(a.block)
                .arg(tb.isValid() && tb.isVisible() ? 1 : 0)
                .arg(a.offset)
                .arg(a.height)
                .arg(visible(tb.text().left(90))));
        return true;
    }
    if (cmd == QLatin1String("react.add")) {
        const QString key  = findKey(str("name", ""));
        auto*         view = b ? m_chat->viewFor(b) : nullptr;
        if (key.isEmpty() || !view || !m_chat->m_reactions) {
            say(QString::fromLatin1("react.add: no entry or chat for ") + str("name", ""));
            return true;
        }
        m_chat->ensurePositions(*view);
        const QVector<int> positions = view->positionsByKey.value(key);
        if (positions.isEmpty()) {
            say(QString::fromLatin1("react.add: no preview of ") + str("name", ""));
            return true;
        }
        ensureShown(b, b->document()->findBlock(positions.first()));
        const QPointer<QTextBrowser> chat(b);
        const auto picture = [this, chat, key]() -> QRectF {
            auto* v = chat ? m_chat->viewFor(chat.data()) : nullptr;
            if (!v || !m_chat->m_reactions)
                return {};
            m_chat->ensurePositions(*v);
            const QVector<int> at = v->positionsByKey.value(key);
            if (at.isEmpty())
                return {};
            return m_chat->m_reactions->pictureRect(chat.data(), key, m_chat->previewRect(chat.data(), at.first(), QSizeF(v->formatSizes.value(key))));
        };
        QWidget* vp = b->viewport();
        sendMouse(vp, QEvent::MouseMove, picture().center().toPoint(), Qt::NoButton, Qt::NoButton);
        const QPointer<QTextBrowser> guard(b);
        later(500, [this, guard, picture, vp] {
            if (!guard) {
                next(0);
                return;
            }
            const QRectF pic    = picture();
            const QPoint button = rx::addButtonRect(pic.size()).translated(pic.topLeft()).center().toPoint();
            sendMouse(vp, QEvent::MouseMove, button, Qt::NoButton, Qt::NoButton);
            sendMouse(vp, QEvent::MouseButtonPress, button, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, button, Qt::LeftButton, Qt::NoButton);
            QWidget* popup = QApplication::activePopupWidget();
            say(QString::fromLatin1("react.add: add button at %1,%2 clicked; popup %3").arg(button.x()).arg(button.y()).arg(popup ? popup->objectName() : QString::fromLatin1("none")));
            next(700);
        });
        return false;
    }
    if (cmd == QLatin1String("react.row")) { // a pill (pill: its index in the row) or the add pill (add: true) under a preview
        const QString key  = findKey(str("name", ""));
        auto*         view = b ? m_chat->viewFor(b) : nullptr;
        ChatReactions* rx  = m_chat->m_reactions;
        if (key.isEmpty() || !view || !rx) {
            say(QString::fromLatin1("react.row: no entry or chat for ") + str("name", ""));
            return true;
        }
        m_chat->ensurePositions(*view);
        const QVector<int> positions = view->positionsByKey.value(key);
        if (positions.isEmpty()) {
            say(QString::fromLatin1("react.row: no preview of ") + str("name", ""));
            return true;
        }
        ensureShown(b, b->document()->findBlock(positions.first()));
        const QRectF object   = m_chat->previewRect(b, positions.first(), QSizeF(view->formatSizes.value(key)));
        const auto   geometry = rx->m_geometry.value(b).value(key);
        if (geometry.row.isEmpty()) {
            say(QString::fromLatin1("react.row: %1 has no reaction row").arg(str("name", "")));
            return true;
        }
        const int    pill = c.value(QString::fromLatin1("pill")).toInt(-1);
        const QRectF zone = pill >= 0 && pill < geometry.row.pills.size() ? geometry.row.pills.at(pill) : geometry.row.addPill;
        const QPoint at   = (object.topLeft() + QPointF(0, geometry.picture.height()) + zone.center()).toPoint();
        QWidget*     vp   = b->viewport();
        sendMouse(vp, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QWidget* popup = QApplication::activePopupWidget();
        say(QString::fromLatin1("react.row: %1 of %2 pills clicked at %3,%4; popup %5")
                .arg(pill >= 0 ? QString::fromLatin1("pill %1").arg(pill) : QString::fromLatin1("add pill"))
                .arg(geometry.row.pills.size())
                .arg(at.x())
                .arg(at.y())
                .arg(popup ? popup->objectName() : QString::fromLatin1("none")));
        next(900);
        return false;
    }
    if (cmd == QLatin1String("voicefake")) { // a voice message from generated sound: voice_fake.txt first, then /tsmedia voice
        QFile fake(ts3::dataDir() + QString::fromLatin1("/voice_fake.txt"));
        if (!fake.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            say(QString::fromLatin1("voicefake: can't write voice_fake.txt; not recording"));
            return true;
        }
        fake.write(str("spec", "4000;send").toUtf8());
        fake.close();
        say(QString::fromLatin1("voicefake: ") + str("spec", "4000;send"));
        ts3plugin_processCommand(sch, "voice");
        next(1000);
        return false;
    }
    if (cmd == QLatin1String("quitin")) { // TeamSpeak's Quit action in ms (the script goes on meanwhile)
        const int ms = c.value(QString::fromLatin1("ms")).toInt(500);
        say(QString::fromLatin1("quitin: %1 ms").arg(ms));
        later(ms, [this] {
            QAction* quit = nullptr;
            if (QWidget* mw = m_chat ? m_chat->mainWindow() : nullptr) {
                for (QAction* a : mw->findChildren<QAction*>()) {
                    if (a->shortcut() == QKeySequence(QString::fromLatin1("Ctrl+Q")))
                        quit = a;
                }
            }
            QWidget* popup = QApplication::activePopupWidget();
            say(QString::fromLatin1("quitin: now, popup %1").arg(popup ? popup->objectName() : QString::fromLatin1("none")));
            if (quit)
                QMetaObject::invokeMethod(quit, "trigger", Qt::QueuedConnection);
        });
        return true;
    }
    if (cmd == QLatin1String("hotkey")) { // as TeamSpeak calls it for a bound hotkey
        const QByteArray keyword = str("keyword", "tsmedia_reply").toLatin1();
        say(QString::fromLatin1("hotkey: ") + QString::fromLatin1(keyword));
        ts3plugin_onHotkeyEvent(keyword.constData());
        next(900);
        return false;
    }
    if (cmd == QLatin1String("selectmsg")) { // selects the text of the nth newest message containing find in the visible chat
        const QTextBlock tb = b ? findMessage(b) : QTextBlock();
        if (!tb.isValid()) {
            say(QString::fromLatin1("selectmsg: no message with \"%1\"").arg(str("find", "")));
            return true;
        }
        const replydoc::Message m = replydoc::parseBlock(tb);
        QTextCursor             cursor(b->document());
        cursor.setPosition(c.value(QString::fromLatin1("whole")).toBool() ? tb.position() : m.textStart);
        cursor.setPosition(tb.position() + tb.length() - 1, QTextCursor::KeepAnchor);
        b->setTextCursor(cursor);
        say(QString::fromLatin1("selectmsg: block %1, %2 characters").arg(tb.blockNumber()).arg(cursor.selectionEnd() - cursor.selectionStart()));
        return true;
    }
    if (cmd == QLatin1String("clickobj")) { // a visible button by objectName (prefix), e.g. the input's emoji button
        auto* button = qobject_cast<QAbstractButton*>(widgetNamed(str("object", "")));
        say(QString::fromLatin1("clickobj: %1").arg(button ? button->objectName() + QString::fromLatin1(" clicked") : QString::fromLatin1("no button ") + str("object", "")));
        if (button)
            button->click();
        next(900);
        return false;
    }
    if (cmd == QLatin1String("reactpicker")) {
        QWidget* picker = topLevelByObject(QString::fromLatin1("tsmediaReactionPicker"));
        if (!picker) {
            say(QString::fromLatin1("reactpicker: none open"));
            return true;
        }
        QList<QAbstractButton*> buttons = picker->findChildren<QAbstractButton*>();
        std::sort(buttons.begin(), buttons.end(), [](QAbstractButton* x, QAbstractButton* y) { return x->x() < y->x(); });
        QStringList names;
        for (QAbstractButton* button : buttons)
            names << button->toolTip() + (button->isChecked() ? QString::fromLatin1("*") : QString());
        say(QString::fromLatin1("reactpicker: %1 buttons [%2]").arg(buttons.size()).arg(names.join(QString::fromLatin1(", "))));
        if (!str("grab", "").isEmpty())
            say(QString::fromLatin1("reactpicker: -> ") + saveGrab(picker, str("grab", "")));
        QAbstractButton* chosen = nullptr;
        if (c.value(QString::fromLatin1("more")).toBool() && !buttons.isEmpty())
            chosen = buttons.last();
        else if (c.contains(QString::fromLatin1("index")) && c.value(QString::fromLatin1("index")).toInt() < buttons.size())
            chosen = buttons.at(c.value(QString::fromLatin1("index")).toInt());
        if (chosen) {
            say(QString::fromLatin1("reactpicker: click \"%1\"").arg(chosen->toolTip()));
            chosen->click();
        }
        next(900);
        return false;
    }
    if (cmd == QLatin1String("emojis")) {
        if (!b) {
            say(QString::fromLatin1("emojis: no visible chat"));
            return true;
        }
        QTextDocument* doc   = b->document();
        int            jumbo = 0, emoticons = 0, hd = 0;
        for (QTextBlock tb = doc->begin(); tb.isValid(); tb = tb.next()) {
            for (auto it = tb.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat f = it.fragment().charFormat();
                if (emojiformat::isHd(f)) {
                    hd += it.fragment().length();
                    jumbo += f.boolProperty(emojiformat::kJumbo) ? it.fragment().length() : 0;
                    emoticons += emojiformat::emoticonName(f).isEmpty() ? 0 : it.fragment().length();
                } else if (f.isImageFormat() && f.toImageFormat().name().startsWith(QLatin1String("emoticons:"))) {
                    say(QString::fromLatin1("emojis: TeamSpeak emoticon left in block %1: %2").arg(tb.blockNumber()).arg(f.toImageFormat().name()));
                }
            }
        }
        say(QString::fromLatin1("emojis: %1 HD (ChatEmoji count %2), %3 jumbo, %4 from TeamSpeak emoticons").arg(hd).arg(ChatEmoji::countEmoji(doc)).arg(jumbo).arg(emoticons));
        return true;
    }
    if (cmd == QLatin1String("setemoji")) {
        Settings& s = Settings::instance();
        if (c.contains(QString::fromLatin1("hdEmoji")))
            s.hdEmoji = c.value(QString::fromLatin1("hdEmoji")).toBool();
        if (c.contains(QString::fromLatin1("jumboEmoji")))
            s.jumboEmoji = c.value(QString::fromLatin1("jumboEmoji")).toBool();
        if (c.contains(QString::fromLatin1("emojiButton")))
            s.emojiButton = c.value(QString::fromLatin1("emojiButton")).toBool();
        s.save();
        m_chat->refreshAll();
        say(QString::fromLatin1("setemoji: hd %1 jumbo %2 button %3").arg(s.hdEmoji).arg(s.jumboEmoji).arg(s.emojiButton));
        next(1500);
        return false;
    }
    *handled = false;
    return true;
}

// ============================================================================================
// Part 4: the chat redesign
//   {"cmd":"layout","mode":"cozy"|"compact"|"classic"|0..2,"group":true,"actions":true,"grab":"name"}
//   {"cmd":"layoutinfo"}                      the visible chat's layout state (and the bar's)
//   {"cmd":"hover","find":"text","nth":0,"at":"body"|"link"|"reply"|"nick"|"media"|"chip"|"leave",
//    "press":true (a left click there),"click":"react"|"reply"|"copy"|"more" (the bar's button),"tip":true,"grab":"name"}
//   {"cmd":"emojibtn","action":"info"|"hover"|"leave"|"press"|"click"|"space"|"close"|"hide"|"show","grab":"name"}
// Part 5 (2.2.1 microphone button and voice messages):
//   {"cmd":"micbtn","action":"info"|"hover"|"leave"|"tip"|"click"|"press"|"move"|"release"|"up",
//    "dx":0,"dy":0,"fake":"<spec>","grab":"name"}
//                                             the visible input's microphone button: its state (and the hold's),
//                                             tool tip and place, the strip above the input, and every button
//                                             around the input (one smiley: TeamSpeak's).
//                                             click: a left press and release on its icon at once (a tap while
//                                             it is Ready: nothing is recorded, the strip shows the hint; stop
//                                             and send while the window records).
//                                             Hold to record: press (at the icon's centre plus dx, dy), move
//                                             (the held pointer to centre plus dx, dy: beyond 72 px is the
//                                             cancel zone), release (there: sends, or cancels in the zone), up
//                                             (the hold's button counts as let go without a release: lost,
//                                             kept in the window after about 120 ms). From press to release or
//                                             up the recorder's check of the physical mouse button reads the
//                                             driver's hold instead (setPointerHeldForTests). Esc while held:
//                                             {"cmd":"key","key":"Escape"}.
//                                             A click or press that would start recording writes voice_fake.txt
//                                             first (fake, default "600000": generated sound, never the
//                                             microphone), and removes it again if nothing started.
//   {"cmd":"voicestate"}                      the recorder: phase, the hold and the strip, the window's view and
//                                             texts, MicGuard, our vars
//   {"cmd":"disconnect","reconnectMs":6000,"probeMs":[1500,4000],"clickMicMs":2500}
//                                             stops this tab's connection and connects it again in the same tab
//                                             (0: stays disconnected for 20 s at most, then the script ends); the
//                                             mic button and the microphone flags are logged at probeMs and after.
//                                             clickMicMs: a click on the mic while recording (the window's: stop
//                                             and send; a hold's: its release, as micbtn release)
// ============================================================================================

bool SelfTest::runPart4(const QString& cmd, const QJsonObject& c, bool* handled)
{
    *handled          = true;
    const auto   str  = [&c](const char* key, const char* fallback) { return c.value(QString::fromLatin1(key)).toString(QString::fromLatin1(fallback)); };
    QTextBrowser* b   = m_chat->visibleChatBrowser();
    ChatLayout*   lay = m_chat->layout();
    const auto findMessage = [&c, &str](QTextBrowser* browser) -> QTextBlock {
        const QString find = str("find", "");
        int           nth  = c.value(QString::fromLatin1("nth")).toInt(0);
        for (QTextBlock tb = browser->document()->lastBlock(); tb.isValid(); tb = tb.previous()) {
            if (!tb.isVisible())
                continue;
            const replydoc::Message m = replydoc::parseBlock(tb);
            if (m.block < 0)
                continue;
            if (!find.isEmpty() && !(m.text + QLatin1Char(' ') + m.mediaLabel).contains(find, Qt::CaseInsensitive))
                continue;
            if (nth-- > 0)
                continue;
            return tb;
        }
        return QTextBlock();
    };
    const auto grabLater = [this, &c, &str](QWidget* w, int ms) {
        const QString        name = str("grab", "");
        const QPointer<QWidget> guard(w);
        if (name.isEmpty() || !w) {
            next(ms);
            return;
        }
        later(ms, [this, guard, name] {
            if (guard)
                say(QString::fromLatin1("grab: -> ") + saveGrab(guard.data(), name));
            next(0);
        });
        Q_UNUSED(c);
    };

    if (cmd == QLatin1String("layout")) {
        Settings&        s    = Settings::instance();
        const QJsonValue mode = c.value(QString::fromLatin1("mode"));
        if (mode.isString()) {
            const QString m = mode.toString();
            s.chatLayout    = m == QLatin1String("classic") ? 0 : m == QLatin1String("compact") ? 2 : 1;
        } else if (mode.isDouble()) {
            s.chatLayout = qBound(Settings::chatLayoutRange.min, mode.toInt(), Settings::chatLayoutRange.max);
        }
        if (c.contains(QString::fromLatin1("group")))
            s.chatGroupMessages = c.value(QString::fromLatin1("group")).toBool();
        if (c.contains(QString::fromLatin1("actions")))
            s.chatHoverActions = c.value(QString::fromLatin1("actions")).toBool();
        s.save();
        QElapsedTimer clock;
        clock.start();
        m_chat->refreshAll();
        if (b && lay)
            lay->processNow(b);
        say(QString::fromLatin1("layout: mode %1 group %2 actions %3 in %4 ms; %5")
                .arg(s.chatLayout)
                .arg(s.chatGroupMessages)
                .arg(s.chatHoverActions)
                .arg(clock.elapsed())
                .arg(b && lay ? lay->describe(b) : QString::fromLatin1("no visible chat")));
        grabLater(b ? b->viewport() : nullptr, c.value(QString::fromLatin1("ms")).toInt(600));
        return false;
    }
    if (cmd == QLatin1String("layoutinfo")) {
        if (!b || !lay) {
            say(QString::fromLatin1("layoutinfo: no visible chat"));
            return true;
        }
        ActionBar* bar = lay->bar();
        say(QString::fromLatin1("layoutinfo: %1; overlay %2; bar %3")
                .arg(lay->describe(b))
                .arg(lay->overlayMode(b) ? 1 : 0)
                .arg(bar && bar->isVisible() ? QString::fromLatin1("shown at %1,%2 %3x%4").arg(bar->x()).arg(bar->y()).arg(bar->width()).arg(bar->height()) : QString::fromLatin1("hidden")));
        return true;
    }
    if (cmd == QLatin1String("hover")) {
        if (!b || !lay) {
            say(QString::fromLatin1("hover: no visible chat"));
            return true;
        }
        QWidget*      vp = b->viewport();
        const QString at = str("at", "body");
        if (at == QLatin1String("leave")) {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(vp, &leave);
            say(QString::fromLatin1("hover: left the chat; bar %1").arg(lay->bar() && lay->bar()->isVisible() ? QString::fromLatin1("still shown") : QString::fromLatin1("hidden")));
            grabLater(vp, 300);
            return false;
        }
        QTextBlock tb = findMessage(b);
        if (at == QLatin1String("chip")) { // a system row: the newest block whose text has find
            tb = QTextBlock();
            for (QTextBlock x = b->document()->lastBlock(); x.isValid() && !tb.isValid(); x = x.previous()) {
                if (x.isVisible() && !layoutdoc::chipsOf(x).isEmpty() && layoutdoc::eventText(x).contains(str("find", ""), Qt::CaseInsensitive))
                    tb = x;
            }
        }
        if (!tb.isValid()) {
            say(QString::fromLatin1("hover: no message with \"%1\"").arg(str("find", "")));
            return true;
        }
        ensureShown(b, tb);
        const int                n      = tb.blockNumber();
        const layoutformat::Lead lead   = layoutformat::leadOf(tb);
        const int                indent = lay->indentFor(b);
        QPoint                   pos;
        if (at == QLatin1String("link")) {
            for (auto it = tb.begin(); !it.atEnd() && pos.isNull(); ++it) {
                const QTextFragment f = it.fragment();
                if (f.isValid() && f.position() >= lead.bodyStart && f.charFormat().isAnchor() && !f.charFormat().isImageFormat()
                    && f.charFormat().anchorHref().startsWith(QLatin1String("http"), Qt::CaseInsensitive))
                    pos = charRect(b, f.position() + qMin(3, f.length() - 1)).center();
            }
        } else if (at == QLatin1String("reply") || at == QLatin1String("nick")) {
            if (!lead.styled || lead.kind != layoutformat::Head) {
                say(QString::fromLatin1("hover: block %1 has no Cozy head").arg(n));
                return true;
            }
            const QRect r = charRect(b, lead.object); // the head picture's line
            pos           = at == QLatin1String("reply") ? QPoint(r.left() + indent + 40, r.top() + 9) : QPoint(r.left() + indent + 8, r.bottom() - 10);
        } else if (at == QLatin1String("media")) {
            const QVector<QRectF> media = m_chat->mediaRectsIn(b, n);
            if (!media.isEmpty())
                pos = media.first().center().toPoint();
        } else if (at == QLatin1String("chip")) { // P2: a run's chip ("+N more events" / "Show fewer") on that row
            const QVector<layoutdoc::ChipRef> chips = layoutdoc::chipsOf(tb);
            if (!chips.isEmpty())
                pos = charRect(b, chips.last().position).center() + QPoint(4, 0);
        } else {
            lay->hoverBlock(b, n, &pos);
        }
        if (pos.isNull()) {
            say(QString::fromLatin1("hover: nothing (%1) to point at in block %2").arg(at).arg(n));
            return true;
        }
        sendMouse(vp, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
        if (c.value(QString::fromLatin1("press")).toBool()) { // a left click there (a reply row, a chip)
            sendMouse(vp, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
        }
        ActionBar*  bar   = lay->bar();
        QStringList shown;
        for (int i = 0; bar && i < 4; ++i) {
            if (QAbstractButton* button = bar->button(i)) {
                if (!button->isHidden())
                    shown << button->toolTip();
            }
        }
        say(QString::fromLatin1("hover: %1 of block %2 at %3,%4 (head kind %5); cursor %6; bar %7 [%8]")
                .arg(at)
                .arg(n)
                .arg(pos.x())
                .arg(pos.y())
                .arg(static_cast<int>(lead.kind))
                .arg(vp->cursor().shape())
                .arg(bar && bar->isVisible() ? QString::fromLatin1("at %1,%2").arg(bar->x()).arg(bar->y()) : QString::fromLatin1("hidden"))
                .arg(shown.join(QString::fromLatin1(", "))));
        if (c.value(QString::fromLatin1("tip")).toBool()) {
            QHelpEvent tip(QEvent::ToolTip, pos, vp->mapToGlobal(pos));
            QApplication::sendEvent(vp, &tip);
            say(QString::fromLatin1("hover: tool tip \"%1\"").arg(QToolTip::isVisible() ? QToolTip::text() : QString()));
        }
        const QString click = str("click", "");
        if (!click.isEmpty()) {
            const int action = click == QLatin1String("react") ? ActionBar::React
                               : click == QLatin1String("reply") ? ActionBar::Reply
                               : click == QLatin1String("copy")  ? ActionBar::Copy
                                                                 : ActionBar::More;
            QAbstractButton* button = bar && bar->isVisible() ? bar->button(action) : nullptr;
            if (!button || button->isHidden()) {
                say(QString::fromLatin1("hover: no %1 button on the bar").arg(click));
            } else {
                QApplication::clipboard()->clear();
                button->click();
                say(QString::fromLatin1("hover: clicked %1").arg(click));
                later(700, [this] {
                    QWidget* popup = QApplication::activePopupWidget();
                    say(QString::fromLatin1("hover: after the click: clipboard \"%1\", popup %2")
                            .arg(visible(QApplication::clipboard()->text().left(200)), popup ? QString::fromLatin1(popup->metaObject()->className()) + QLatin1Char('#') + popup->objectName() : QString::fromLatin1("none")));
                });
            }
        }
        grabLater(vp, c.value(QString::fromLatin1("ms")).toInt(click.isEmpty() ? 300 : 1200));
        return false;
    }
    if (cmd == QLatin1String("emojibtn")) {
        EmojiInput* ei   = m_chat->emoji() ? m_chat->emoji()->input() : nullptr;
        QTextEdit*  edit = nullptr;
        for (const QPointer<QWidget>& w : m_chat->m_inputs) {
            if (w && w->isVisible() && qobject_cast<QTextEdit*>(w.data()))
                edit = qobject_cast<QTextEdit*>(w.data());
        }
        if (!ei || !edit) {
            say(QString::fromLatin1("emojibtn: no chat input"));
            return true;
        }
        QAbstractButton* taken    = ei->takenButton(edit);
        QAbstractButton* fallback = ei->fallbackButton(edit);
        QAbstractButton* found    = EmojiInput::findTeamSpeakButton(edit);
        QAbstractButton* button   = taken ? taken : found;
        const QString    action   = str("action", "info");
        const auto       displays = [] {
            int n = 0;
            for (QWidget* w : QApplication::topLevelWidgets())
                n += w->isVisible() && w->inherits("EmoticonsDisplay") ? 1 : 0;
            return n;
        };
        say(QString::fromLatin1("emojibtn: %1").arg(ei->describe()));
        if (action == QLatin1String("hover") && button) {
            button->setAttribute(Qt::WA_UnderMouse, true);
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(button, &enter);
            button->update();
        } else if (action == QLatin1String("leave") && button) {
            button->setAttribute(Qt::WA_UnderMouse, false);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(button, &leave);
            button->update();
        } else if (action == QLatin1String("press") && button) {
            sendMouse(button, QEvent::MouseButtonPress, button->rect().center(), Qt::LeftButton, Qt::LeftButton);
            const QPointer<QAbstractButton> guard(button);
            later(1500, [guard] { // let go outside: nothing opens
                if (guard)
                    sendMouse(guard.data(), QEvent::MouseButtonRelease, QPoint(-40, -40), Qt::LeftButton, Qt::NoButton);
            });
        } else if (action == QLatin1String("click") && button) {
            sendMouse(button, QEvent::MouseButtonPress, button->rect().center(), Qt::LeftButton, Qt::LeftButton);
            sendMouse(button, QEvent::MouseButtonRelease, button->rect().center(), Qt::LeftButton, Qt::NoButton);
        } else if (action == QLatin1String("space") && button) {
            button->setFocus(Qt::TabFocusReason);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QString::fromLatin1(" "));
            QApplication::sendEvent(button, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QString::fromLatin1(" "));
            QApplication::sendEvent(button, &release);
        } else if (action == QLatin1String("close")) {
            if (QWidget* popup = QApplication::activePopupWidget())
                popup->close();
        } else if (action == QLatin1String("hide") && button) {
            button->hide(); // as TeamSpeak's emoticon option would (test only; "show" gives it back)
        } else if (action == QLatin1String("show") && button) {
            button->show();
        }
        const QPointer<QTextEdit> guardEdit(edit);
        later(500, [this, ei, guardEdit, displays] {
            if (!m_chat || !guardEdit)
                return;
            say(QString::fromLatin1("emojibtn: after: taken %1, fallback %2, picker %3, EmoticonsDisplay shown %4, tool tip \"%5\"")
                    .arg(ei->takenButton(guardEdit.data()) ? 1 : 0)
                    .arg(ei->fallbackButton(guardEdit.data()) ? 1 : 0)
                    .arg(ei->pickerOpen() ? 1 : 0)
                    .arg(displays())
                    .arg(ei->takenButton(guardEdit.data()) ? ei->takenButton(guardEdit.data())->toolTip() : QString()));
        });
        Q_UNUSED(fallback);
        QWidget* area = button ? button->parentWidget() : edit->parentWidget();
        grabLater(area, c.value(QString::fromLatin1("ms")).toInt(900));
        return false;
    }

    // ---- part 5: the microphone button and voice messages -------------------------------------------
    const auto visibleInput = [this]() -> QTextEdit* {
        for (const QPointer<QWidget>& w : m_chat->m_inputs) {
            if (w && w->isVisible() && qobject_cast<QTextEdit*>(w.data()))
                return qobject_cast<QTextEdit*>(w.data());
        }
        return nullptr;
    };
    const QPointer<ChatIntegration> chat = m_chat;
    const auto phaseName = [chat]() -> QString {
        VoiceController* voice = chat ? chat->voice() : nullptr;
        if (!voice)
            return QString::fromLatin1("no controller");
        static const char* names[] = {"Idle", "Starting", "Recording", "Saving", "TooShort", "Error"};
        return QString::fromLatin1(names[static_cast<int>(voice->phase())]);
    };
    const auto micState = [chat, phaseName](QTextEdit* edit) -> QString {
        MicButton* mic = edit ? MicButton::of(edit) : nullptr;
        if (!mic)
            return QString::fromLatin1("no mic button (input %1); phase %2").arg(edit ? 1 : 0).arg(phaseName());
        static const char* modes[] = {"Ready", "Unavailable", "Holding", "Recording", "Busy"}; // MicButton::Mode
        const QRect icon = mic->iconRect().translated(mic->pos());
        return QString::fromLatin1("mic %1 enabled %2 visible %3 tip \"%4\" reason \"%5\" a11y \"%6\"/\"%7\" pulse %8 cursor %9; "
                                   "at %10,%11 %12x%13 icon %14,%15 in input %16x%17 (viewport right %18); shared %19; phase %20")
            .arg(QString::fromLatin1(modes[static_cast<int>(mic->state().mode)]))
            .arg(mic->isEnabled() ? 1 : 0)
            .arg(mic->isVisible() ? 1 : 0)
            .arg(mic->toolTip(), mic->state().reason, mic->accessibleName(), mic->accessibleDescription())
            .arg(mic->pulsing() ? 1 : 0)
            .arg(mic->cursor().shape())
            .arg(mic->x())
            .arg(mic->y())
            .arg(mic->width())
            .arg(mic->height())
            .arg(icon.x())
            .arg(icon.y())
            .arg(edit->width())
            .arg(edit->height())
            .arg(edit->viewport()->geometry().right())
            .arg(QString::fromLatin1(modes[static_cast<int>(MicButton::sharedState().mode)]))
            .arg(phaseName())
            + QString::fromLatin1("; hold %1 cancelZone %2 down %3 cancel %4")
                  .arg(mic->holding() ? 1 : 0)
                  .arg(mic->inCancelZone() ? 1 : 0)
                  .arg(mic->isDown() ? 1 : 0)
                  .arg(mic->state().cancel ? 1 : 0);
    };
    // The strip above the input while the button is held (and its notice afterwards).
    const auto stripState = [chat]() -> QString {
        VoiceController* voice = chat ? chat->voice() : nullptr;
        HoldStrip*       strip = voice ? voice->strip() : nullptr;
        if (!strip)
            return QString::fromLatin1("no strip");
        static const char*     modes[] = {"Recording", "Cancel", "Saving", "Notice"}; // HoldStrip::Mode
        const HoldStrip::View& v       = strip->view();
        QWidget*               owner   = strip->parentWidget();
        QWidget*               input   = strip->input();
        const QRect            in      = input && owner ? QRect(input->mapTo(owner, QPoint(0, 0)), input->size()) : QRect();
        return QString::fromLatin1("strip visible %1 mode %2 time %3 ms warning %4 line \"%5\" at %6,%7 %8x%9 (input at %10,%11 %12 wide, visible %13) "
                                   "parent %14#%15 on top %16 quitOnClose %17")
            .arg(strip->isVisible() ? 1 : 0)
            .arg(QString::fromLatin1(modes[static_cast<int>(v.mode)]))
            .arg(v.timeMs)
            .arg(v.timeWarning ? 1 : 0)
            .arg(HoldStrip::line(v))
            .arg(strip->x())
            .arg(strip->y())
            .arg(strip->width())
            .arg(strip->height())
            .arg(in.x())
            .arg(in.y())
            .arg(in.width())
            .arg(input && input->isVisible() ? 1 : 0)
            .arg(owner ? QString::fromLatin1(owner->metaObject()->className()) : QString::fromLatin1("none"), owner ? owner->objectName() : QString())
            .arg(owner && !owner->children().isEmpty() && owner->children().last() == strip ? 1 : 0)
            .arg(strip->testAttribute(Qt::WA_QuitOnClose) ? 1 : 0);
    };
    // Every visible button in the input's row (the input's parent): one smiley (TeamSpeak's) and our mic.
    const auto rowButtons = [](QTextEdit* edit) -> QString {
        QWidget* row = edit ? edit->parentWidget() : nullptr;
        if (!row)
            return QString::fromLatin1("no row");
        QStringList out;
        for (QAbstractButton* b : row->findChildren<QAbstractButton*>()) {
            if (!b->isVisible())
                continue;
            const QPoint at = b->mapTo(row, QPoint(0, 0));
            out << QString::fromLatin1("%1#%2 %3,%4 %5x%6 tip \"%7\"")
                       .arg(QString::fromLatin1(b->metaObject()->className()), b->objectName())
                       .arg(at.x())
                       .arg(at.y())
                       .arg(b->width())
                       .arg(b->height())
                       .arg(b->toolTip().left(80));
        }
        const QPoint inputAt = edit->mapTo(row, QPoint(0, 0));
        return QString::fromLatin1("row %1#%2 %3x%4, input at %5,%6 %7x%8: %9 visible buttons [%10]")
            .arg(QString::fromLatin1(row->metaObject()->className()), row->objectName())
            .arg(row->width())
            .arg(row->height())
            .arg(inputAt.x())
            .arg(inputAt.y())
            .arg(edit->width())
            .arg(edit->height())
            .arg(out.size())
            .arg(out.join(QString::fromLatin1("; ")));
    };
    const auto flags = [](uint64 sch) -> QString {
        int muted = -1, deact = -1, hw = -1;
        const unsigned e = ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, &muted);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, &deact);
        ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &hw);
        return QString::fromLatin1("sch %1 connected %2: input_muted %3 input_deactivated %4 input_hardware %5 (error %6)")
            .arg(sch)
            .arg(ts3::isConnected(sch) ? 1 : 0)
            .arg(muted)
            .arg(deact)
            .arg(hw)
            .arg(e);
    };
    const auto voiceState = [chat, phaseName, stripState]() -> QString {
        VoiceController* voice = chat ? chat->voice() : nullptr;
        if (!voice)
            return QString::fromLatin1("no controller");
        VoicePanel* panel = voice->panel();
        QString     out   = QString::fromLatin1("phase %1 holding %2; %3").arg(phaseName()).arg(voice->holding() ? 1 : 0).arg(stripState());
        if (panel) {
            static const char* modes[] = {"Starting", "Recording", "Saving", "TooShort", "Error"};
            const VoicePanel::View& v  = panel->view();
            out += QString::fromLatin1("; window visible %1 active %2 %3x%4 mode %5 time %6 ms warning %7 confirm %8 sendingShown %9 sending %10 "
                                       "target \"%11\" hints [%12] error \"%13\" / \"%14\" fix %15 \"%16\"; texts: %17")
                       .arg(panel->isVisible() ? 1 : 0)
                       .arg(panel->isActiveWindow() ? 1 : 0)
                       .arg(panel->width())
                       .arg(panel->height())
                       .arg(QString::fromLatin1(modes[static_cast<int>(v.mode)]))
                       .arg(v.timeMs)
                       .arg(v.timeWarning ? 1 : 0)
                       .arg(v.confirmDiscard ? 1 : 0)
                       .arg(v.sendingShown ? 1 : 0)
                       .arg(v.sending ? 1 : 0)
                       .arg(v.target, v.hints.join(QString::fromLatin1(" | ")), v.errorTitle, v.errorBody)
                       .arg(static_cast<int>(v.fix))
                       .arg(v.fixText, describeWidgetTexts(panel));
        } else {
            out += QString::fromLatin1("; no window");
        }
        out += QString::fromLatin1("; diag: ") + voice->diagnosticLines().join(QString::fromLatin1(" / "));
        return out;
    };

    if (cmd == QLatin1String("micbtn")) {
        QTextEdit*    edit   = visibleInput();
        MicButton*    mic    = edit ? MicButton::of(edit) : nullptr;
        const QString action = str("action", "info");
        const QPoint  offset(c.value(QString::fromLatin1("dx")).toInt(0), c.value(QString::fromLatin1("dy")).toInt(0)); // from the icon's centre
        say(QString::fromLatin1("micbtn: %1").arg(micState(edit)));
        say(QString::fromLatin1("micbtn: %1").arg(stripState()));
        say(QString::fromLatin1("micbtn: %1").arg(rowButtons(edit)));
        if (mic && action == QLatin1String("hover")) {
            const QPoint at = mic->iconRect().center();
            mic->setAttribute(Qt::WA_UnderMouse, true);
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(mic, &enter);
            sendMouse(mic, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
            mic->update();
        } else if (mic && action == QLatin1String("leave")) {
            mic->setAttribute(Qt::WA_UnderMouse, false);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(mic, &leave);
            mic->update();
        } else if (mic && action == QLatin1String("tip")) {
            const QPoint at = mic->iconRect().center();
            QHelpEvent   tip(QEvent::ToolTip, at, mic->mapToGlobal(at));
            QApplication::sendEvent(mic, &tip);
            say(QString::fromLatin1("micbtn: tool tip shown %1 \"%2\"").arg(QToolTip::isVisible() ? 1 : 0).arg(QToolTip::isVisible() ? QToolTip::text() : QString()));
        } else if (mic && (action == QLatin1String("click") || action == QLatin1String("press"))) {
            // A press records at once (a click is a tap: it records, then discards): never the real
            // microphone, generated sound for whatever this starts.
            VoiceController* voice  = m_chat->voice();
            const bool       starts = voice && (voice->phase() == VoiceController::Phase::Idle || voice->phase() == VoiceController::Phase::TooShort) && mic->isEnabled();
            const QString    fake   = ts3::dataDir() + QString::fromLatin1("/voice_fake.txt");
            if (starts) {
                QFile file(fake);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    say(QString::fromLatin1("micbtn: can't write voice_fake.txt; not pressing"));
                    return true;
                }
                file.write(str("fake", "600000").toUtf8());
                file.close();
                say(QString::fromLatin1("micbtn: voice_fake.txt = ") + str("fake", "600000"));
            }
            const QPoint at = mic->iconRect().center() + offset;
            if (action == QLatin1String("press")) {
                // The recorder checks the physical button while held (it is up: these events are ours): it
                // reads this hold instead until release or up.
                m_micHeld = true;
                if (voice) {
                    const QPointer<SelfTest> self(this);
                    voice->setPointerHeldForTests([self] { return self && self->m_micHeld; });
                }
                sendMouse(mic, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
                sendMouse(mic, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                say(QString::fromLatin1("micbtn: pressed at %1,%2 (enabled %3): button hold %4, recorder holding %5, phase %6")
                        .arg(at.x())
                        .arg(at.y())
                        .arg(mic->isEnabled() ? 1 : 0)
                        .arg(mic->holding() ? 1 : 0)
                        .arg(voice && voice->holding() ? 1 : 0)
                        .arg(phaseName()));
            } else {
                sendMouse(mic, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
                sendMouse(mic, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                sendMouse(mic, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                say(QString::fromLatin1("micbtn: clicked at %1,%2 (enabled %3)").arg(at.x()).arg(at.y()).arg(mic->isEnabled() ? 1 : 0));
            }
            later(1500, [fake, phaseName] {
                if (QFile::exists(fake) && phaseName() == QLatin1String("Idle")) {
                    QFile::remove(fake); // nothing started: no fake left for later
                    say(QString::fromLatin1("micbtn: nothing started, voice_fake.txt removed"));
                }
            });
        } else if (mic && action == QLatin1String("move")) {
            const QPoint at = mic->iconRect().center() + offset;
            sendMouse(mic, QEvent::MouseMove, at, Qt::NoButton, m_micHeld ? Qt::LeftButton : Qt::NoButton);
            say(QString::fromLatin1("micbtn: moved to %1,%2 (%3 px from the icon's centre): hold %4 cancelZone %5")
                    .arg(at.x())
                    .arg(at.y())
                    .arg(qRound(std::hypot(static_cast<double>(offset.x()), static_cast<double>(offset.y()))))
                    .arg(mic->holding() ? 1 : 0)
                    .arg(mic->inCancelZone() ? 1 : 0));
        } else if (action == QLatin1String("release") || action == QLatin1String("up")) {
            VoiceController* voice   = m_chat->voice();
            const bool       wasHeld = mic && mic->holding();
            m_micHeld                = false;
            if (action == QLatin1String("release")) {
                if (mic) {
                    const QPoint at = mic->iconRect().center() + offset;
                    sendMouse(mic, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                    say(QString::fromLatin1("micbtn: released at %1,%2 (button hold was %3): phase %4").arg(at.x()).arg(at.y()).arg(wasHeld ? 1 : 0).arg(phaseName()));
                }
                if (voice)
                    voice->setPointerHeldForTests(&VoiceController::primaryButtonHeld); // the real mouse again
            } else {
                // Up without a release (let go where TeamSpeak didn't see it): the recorder finds it after
                // about 120 ms and keeps the recording in the window. The real mouse again after that.
                say(QString::fromLatin1("micbtn: up without a release (button hold %1)").arg(wasHeld ? 1 : 0));
                later(1000, [this] {
                    if (!m_micHeld && m_chat && m_chat->voice())
                        m_chat->voice()->setPointerHeldForTests(&VoiceController::primaryButtonHeld);
                });
            }
        }
        const QPointer<QTextEdit> guard(edit);
        later(c.value(QString::fromLatin1("afterMs")).toInt(600), [this, guard, micState, stripState] {
            if (m_chat) {
                say(QString::fromLatin1("micbtn: after: %1").arg(micState(guard.data())));
                say(QString::fromLatin1("micbtn: after: %1").arg(stripState()));
            }
        });
        grabLater(mic ? mic->parentWidget()->parentWidget() : nullptr, c.value(QString::fromLatin1("ms")).toInt(900));
        return false;
    }
    if (cmd == QLatin1String("voicestate")) {
        say(QString::fromLatin1("voicestate: %1").arg(voiceState()));
        say(QString::fromLatin1("voicestate: %1").arg(flags(ts3::currentConnection())));
        return true;
    }
    if (cmd == QLatin1String("disconnect")) {
        const uint64 sch = ts3::currentConnection();
        char*        nickRaw = nullptr;
        QByteArray   nick    = "TesterA";
        if (ts3::funcs.getClientSelfVariableAsString && ts3::funcs.getClientSelfVariableAsString(sch, CLIENT_NICKNAME, &nickRaw) == ERROR_ok)
            nick = ts3::takeString(nickRaw).toUtf8();
        say(QString::fromLatin1("disconnect: before: %1; %2").arg(flags(sch), micState(visibleInput())));
        const unsigned err = ts3::funcs.stopConnection(sch, "");
        say(QString::fromLatin1("disconnect: stopConnection(%1) error %2").arg(sch).arg(err));
        QList<int> probes;
        for (const QJsonValue& v : c.value(QString::fromLatin1("probeMs")).toArray())
            probes << v.toInt();
        for (const int ms : probes) {
            later(ms, [ms, sch, flags, micState, visibleInput, voiceState] {
                say(QString::fromLatin1("disconnect: +%1 ms: %2; %3").arg(ms).arg(flags(sch), micState(visibleInput())));
                say(QString::fromLatin1("disconnect: +%1 ms: %2").arg(ms).arg(voiceState()));
            });
        }
        // clickMicMs: a click on the mic button meanwhile (only while recording: it stops and sends, or keeps it)
        const int clickMicMs = c.value(QString::fromLatin1("clickMicMs")).toInt(-1);
        if (clickMicMs >= 0) {
            later(clickMicMs, [this, clickMicMs, visibleInput, micState] {
                VoiceController* voice = m_chat ? m_chat->voice() : nullptr;
                QTextEdit*       edit  = visibleInput();
                MicButton*       mic   = edit ? MicButton::of(edit) : nullptr;
                if (!voice || !mic || voice->phase() != VoiceController::Phase::Recording) {
                    say(QString::fromLatin1("disconnect: +%1 ms: no click (not recording): %2").arg(clickMicMs).arg(micState(edit)));
                    return;
                }
                const QPoint at   = mic->iconRect().center();
                const bool   held = mic->holding(); // a hold: this click's release is the hold's release
                sendMouse(mic, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                sendMouse(mic, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                if (held) {
                    m_micHeld = false;
                    voice->setPointerHeldForTests(&VoiceController::primaryButtonHeld);
                }
                say(QString::fromLatin1("disconnect: +%1 ms: mic clicked (hold %2): %3").arg(clickMicMs).arg(held ? 1 : 0).arg(micState(edit)));
            });
        }
        const int reconnectMs = c.value(QString::fromLatin1("reconnectMs")).toInt(6000);
        if (reconnectMs > 0) {
            later(reconnectMs, [sch, nick, flags] {
                uint64         tab = 0;
                const unsigned e   = ts3::funcs.guiConnect(PLUGIN_CONNECT_TAB_CURRENT, nick.constData(), "127.0.0.1:9987", "", nick.constData(), "", "", "", "", "", "", "", "", "", &tab);
                say(QString::fromLatin1("disconnect: reconnect in the current tab: error %1 tab %2 (was %3); %4").arg(e).arg(tab).arg(sch).arg(flags(sch)));
            });
        }
        next(c.value(QString::fromLatin1("ms")).toInt(800));
        return false;
    }
    *handled = false;
    return true;
}

#endif
