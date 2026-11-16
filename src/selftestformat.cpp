// Test builds only (TSMEDIA_TESTHOOKS): the live-test driver's commands for 2.7 F1 (formatting in the chat input and
// text spoilers in the chats; selftest.cpp hands them over). Logged as "[test] script: ...".
//   {"cmd":"input.set","text":"Hello world","from":6,"to":11}  the visible chat input's text and selection (focused)
//   {"cmd":"input.state"}                     its text, selection and whether TeamSpeak's placeholder shows
//   {"cmd":"input.menu"}                      a keyboard context menu on it, posted (the menu runs its own loop)
//   {"cmd":"menu.dump"}                       the open popup menu: each entry (text, key text, tick, enabled), submenus
//                                             indented
//   {"cmd":"menu.trigger","text":"Bold"}      the open popup menu's entry (or a submenu's) with that text (the first
//                                             with it as a prefix otherwise), triggered, then the menu closed
//   {"cmd":"chat.spoilers"}                   the visible chat's text spoilers: block, id, state, bars (viewport rects)
//   {"cmd":"spoiler.click","index":0}         a click on the index-th bar in the visible chat (move, press, release on
//                                             the viewport: every filter on the way sees it)
// The paint meter: on every chat viewport, a Paint that shows a raw pair the module would hide logs
// "[test] spoiler: raw pair painted" (critique H2: must never appear).

#ifdef TSMEDIA_TESTHOOKS

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>

#include <functional>

#include "chattextspoilers.h"
#include "inputformat.h"
#include "ts3api.h"

namespace {

// Logs a raw pair that is about to be painted (once per chat until the chat shows none again).
class PaintMeter : public QObject
{
  public:
    PaintMeter(ChatTextSpoilers* spoilers, QTextBrowser* browser)
        : QObject(spoilers)
        , m_spoilers(spoilers)
        , m_browser(browser)
    {
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::Paint || !m_browser || watched != m_browser->viewport())
            return false;
        const int raw = m_spoilers->rawPairsShown(m_browser.data());
        if (raw > 0 && !m_logged) // 2.7 live: whether the 0 ms pass was still queued then (a paint ran ahead of it)
            ts3::log(QString::fromLatin1("[test] spoiler: raw pair painted (%1 in view; early pass %2)")
                         .arg(raw)
                         .arg(m_spoilers->earlyPending() ? QString::fromLatin1("queued") : QString::fromLatin1("not queued")));
        m_logged = raw > 0;
        return false;
    }

  private:
    ChatTextSpoilers*      m_spoilers;
    QPointer<QTextBrowser> m_browser;
    bool                   m_logged = false;
};

QTextEdit* visibleInput(const QList<QPointer<QWidget>>& inputs)
{
    QTextEdit* any = nullptr;
    for (const QPointer<QWidget>& w : inputs) {
        auto* edit = qobject_cast<QTextEdit*>(w.data());
        if (!edit)
            continue;
        if (edit->isVisible())
            return edit;
        any = any ? any : edit;
    }
    return any;
}

QString plainText(const QAction* a)
{
    QString text = a->text().section(QLatin1Char('\t'), 0, 0);
    text.remove(QLatin1Char('&'));
    return text;
}

void dumpMenu(const QMenu* menu, int depth, QStringList* out)
{
    for (const QAction* a : menu->actions()) {
        const QString indent(depth * 2, QLatin1Char(' '));
        if (a->isSeparator()) {
            *out << indent + QString::fromLatin1("---");
            continue;
        }
        QString line = indent + plainText(a);
        const QString keys = a->text().section(QLatin1Char('\t'), 1);
        if (!keys.isEmpty())
            line += QString::fromLatin1(" [text key %1]").arg(keys);
        if (!a->shortcut().isEmpty())
            line += QString::fromLatin1(" [shortcut %1]").arg(a->shortcut().toString(QKeySequence::PortableText));
        if (a->isCheckable())
            line += a->isChecked() ? QString::fromLatin1(" (ticked)") : QString::fromLatin1(" (not ticked)");
        if (!a->isEnabled())
            line += QString::fromLatin1(" (disabled)");
        *out << line;
        if (a->menu())
            dumpMenu(a->menu(), depth + 1, out);
    }
}

QAction* findAction(QMenu* menu, const QString& text, bool prefix)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator())
            continue;
        const QString t = plainText(a);
        if (!a->menu() && (t == text || (prefix && t.startsWith(text, Qt::CaseInsensitive))))
            return a;
        if (a->menu()) {
            if (QAction* inner = findAction(a->menu(), text, prefix))
                return inner;
        }
    }
    return nullptr;
}

void sendMouse(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(pos), QPointF(w->mapTo(w->window(), pos)), QPointF(w->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

} // namespace

// chattextspoilers.cpp: the meter for a chat it attaches to (a child of the module).
QObject* selfTestFormatMeter(ChatTextSpoilers* spoilers, QTextBrowser* browser)
{
    return spoilers && browser ? new PaintMeter(spoilers, browser) : nullptr;
}

bool selfTestFormat(const QString& cmd, const QJsonObject& c, ChatTextSpoilers* spoilers, InputFormat* format, QTextBrowser* chat,
                    const QList<QPointer<QWidget>>& inputs, const std::function<void(const QString&)>& say)
{
    Q_UNUSED(format)
    const auto str = [&c](const char* key) { return c.value(QString::fromLatin1(key)).toString(); };
    const auto num = [&c](const char* key, int fallback) { return c.value(QString::fromLatin1(key)).toInt(fallback); };
    const auto inputState = [](QTextEdit* edit) {
        const QTextCursor cursor = edit->textCursor();
        return QString::fromLatin1("text \"%1\" selection %2..%3 placeholder %4")
            .arg(edit->toPlainText())
            .arg(cursor.selectionStart())
            .arg(cursor.selectionEnd())
            .arg(edit->property("helpTextVisible").toBool() ? 1 : 0);
    };

    if (cmd == QLatin1String("input.set") || cmd == QLatin1String("input.state") || cmd == QLatin1String("input.menu")) {
        QTextEdit* edit = visibleInput(inputs);
        if (!edit) {
            say(cmd + QString::fromLatin1(": no chat input"));
            return true;
        }
        if (cmd == QLatin1String("input.set")) {
            edit->window()->activateWindow();
            edit->setFocus(Qt::OtherFocusReason);
            edit->setPlainText(str("text"));
            const int   last = qMax(0, edit->document()->characterCount() - 1);
            QTextCursor cursor(edit->document());
            cursor.setPosition(qBound(0, num("from", last), last));
            cursor.setPosition(qBound(0, num("to", last), last), QTextCursor::KeepAnchor);
            edit->setTextCursor(cursor);
        } else if (cmd == QLatin1String("input.menu")) {
            const QPoint at = edit->cursorRect().center();
            QCoreApplication::postEvent(edit, new QContextMenuEvent(QContextMenuEvent::Keyboard, at, edit->mapToGlobal(at)));
            say(QString::fromLatin1("input.menu: posted"));
            return true;
        }
        say(cmd + QString::fromLatin1(": ") + inputState(edit));
        return true;
    }
    if (cmd == QLatin1String("menu.dump") || cmd == QLatin1String("menu.trigger")) {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            say(cmd + QString::fromLatin1(": no menu open"));
            return true;
        }
        if (cmd == QLatin1String("menu.dump")) {
            QStringList lines;
            dumpMenu(menu, 0, &lines);
            say(QString::fromLatin1("menu.dump: %1 entries%2").arg(menu->actions().size()).arg(menu->parentWidget() && menu->parentWidget()->inherits("ChatLineEdit")
                                                                                               ? QString::fromLatin1(" (the chat input's)")
                                                                                               : QString()));
            for (const QString& line : qAsConst(lines))
                say(QString::fromLatin1("menu.dump:   ") + line);
            return true;
        }
        const QString text   = str("text");
        QAction*      action = findAction(menu, text, false);
        if (!action)
            action = findAction(menu, text, true);
        if (!action) {
            say(QString::fromLatin1("menu.trigger: no entry \"%1\"").arg(text));
            return true;
        }
        const QString name = plainText(action);
        action->trigger(); // the input's selection is still there (as at a user's click)
        menu->close();
        QTextEdit* edit = visibleInput(inputs);
        say(QString::fromLatin1("menu.trigger: \"%1\"%2").arg(name, edit ? QString::fromLatin1(", input ") + inputState(edit) : QString()));
        return true;
    }
    if (cmd == QLatin1String("chat.spoilers")) {
        if (!spoilers || !chat) {
            say(QString::fromLatin1("chat.spoilers: no chat"));
            return true;
        }
        const QStringList lines = spoilers->describe(chat);
        say(QString::fromLatin1("chat.spoilers: %1 spoiler(s), %2 raw pair(s) in view; %3")
                .arg(lines.size())
                .arg(spoilers->rawPairsShown(chat))
                .arg(spoilers->diagnosticLines().join(QLatin1String("; "))));
        for (const QString& line : lines)
            say(QString::fromLatin1("chat.spoilers:   ") + line);
        return true;
    }
    if (cmd == QLatin1String("spoiler.click")) {
        if (!spoilers || !chat) {
            say(QString::fromLatin1("spoiler.click: no chat"));
            return true;
        }
        const QRect    view = chat->viewport()->rect();
        QVector<QRect> bars;
        for (const QRect& r : spoilers->barRects(chat)) {
            if (view.contains(r.center()))
                bars << r;
        }
        const int index = num("index", 0);
        if (index < 0 || index >= bars.size()) {
            say(QString::fromLatin1("spoiler.click: no bar %1 (%2 in view)").arg(index).arg(bars.size()));
            return true;
        }
        const QPoint at = bars.at(index).center();
        QWidget*     vp = chat->viewport();
        const int    id = spoilers->barAt(chat, at);
        sendMouse(vp, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        say(QString::fromLatin1("spoiler.click: bar %1 at %2,%3 (spoiler %4); %5")
                .arg(index)
                .arg(at.x())
                .arg(at.y())
                .arg(id)
                .arg(spoilers->diagnosticLines().join(QLatin1String("; "))));
        return true;
    }
    return false;
}

#endif // TSMEDIA_TESTHOOKS
