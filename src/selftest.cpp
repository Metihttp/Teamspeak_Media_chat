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

#ifdef TSMEDIA_TESTHOOKS

#include "selftest.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
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
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>

#include <functional>

#include "chatintegration.h"
#include "composedialog.h"
#include "core.h"
#include "inlinemedia.h"
#include "settings.h"
#include "ts3api.h"

namespace {

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
        auto*              t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t, deadline] {
            t->deleteLater();
            waitIdle(deadline);
        });
        t->start(500);
        return;
    }
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
            for (QTabBar* bar : mw->findChildren<QTabBar*>()) {
                if (!bar->isVisible())
                    continue; // a hidden server tab's chat tabs
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
        QAbstractButton* target = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (!top->isVisible() || !top->windowTitle().contains(title, Qt::CaseInsensitive))
                continue;
            for (QAbstractButton* b : top->findChildren<QAbstractButton*>()) {
                if (!b->isVisible() || b->text() != text)
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
        say(QString::fromLatin1("click: \"%1\" in \"%2\"%3").arg(text, title, target ? QString() : QString::fromLatin1(" not found")));
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
        else if (key == QLatin1String("uploadMaxMB"))
            s.uploadMaxMB = v.toInt();
        else if (key == QLatin1String("uploadDirectory"))
            s.uploadDirectory = Settings::normalizeUploadDirectory(v.toString()); // Qt-allocated (from the JSON)
        else {
            say(QString::fromLatin1("setting: unknown key ") + key);
            return true;
        }
        s.save();
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
    say(QString::fromLatin1("unknown command ") + cmd);
    return true;
}

#endif
