#include "inputformat.h"

#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>

#include "i18n.h"

namespace {

constexpr textformat::Format kTop[4] = {textformat::Format::Bold, textformat::Format::Italic, textformat::Format::Underline, textformat::Format::Spoiler};

QKeySequence altShift(int key)
{
    return QKeySequence(static_cast<int>(Qt::ALT | Qt::SHIFT) | key);
}

// The text after the tab of an action's text read as keys (Qt's own menu shows "Select All\tCtrl+A" without a
// shortcut()); empty: none.
QKeySequence keyText(const QAction* action)
{
    const QString text = action->text();
    const int     tab  = text.lastIndexOf(QLatin1Char('\t'));
    if (tab < 0)
        return QKeySequence();
    const QString keys = text.mid(tab + 1).trimmed();
    QKeySequence  seq  = QKeySequence::fromString(keys, QKeySequence::NativeText);
    if (seq.isEmpty())
        seq = QKeySequence::fromString(keys, QKeySequence::PortableText);
    return seq;
}

bool hasKey(const QAction* action, const QKeySequence& key)
{
    if (!action || action->isSeparator())
        return false;
    return (!action->shortcut().isEmpty() && action->shortcut() == key) || keyText(action) == key;
}

// "Select All", found by its keys (any language), or by the name Qt's own text menus give it.
bool isSelectAll(const QAction* action)
{
    if (!action || action->isSeparator() || action->menu())
        return false;
    for (const QKeySequence& key : QKeySequence::keyBindings(QKeySequence::SelectAll)) {
        if (hasKey(action, key))
            return true;
    }
    return action->objectName() == QLatin1String("select-all");
}

// TeamSpeak's Format submenu (found by its actions' keys, not its title) and its Underline action.
QMenu* formatMenuOf(QMenu* menu, QAction** submenuAction, QAction** underline)
{
    for (QAction* a : menu->actions()) {
        QMenu* sub = a->menu();
        if (!sub)
            continue;
        bool bold = false;
        for (QAction* s : sub->actions()) {
            if (hasKey(s, altShift(Qt::Key_B)))
                bold = true;
            if (hasKey(s, altShift(Qt::Key_U)))
                *underline = s;
        }
        if (bold) {
            *submenuAction = a;
            return sub;
        }
        *underline = nullptr;
    }
    return nullptr;
}

} // namespace

// Catches the input's menu while it is built (Polish comes before Show), armed for one context-menu event only.
class InputFormat::Catcher : public QObject
{
  public:
    explicit Catcher(InputFormat* owner)
        : QObject(owner)
        , m_owner(owner)
    {
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        const QEvent::Type type = event->type();
        if (type == QEvent::Polish || type == QEvent::Show) {
            if (auto* menu = qobject_cast<QMenu*>(watched)) {
                QWidget* parent = menu->parentWidget();
                if (parent && parent->inherits("ChatLineEdit"))
                    m_owner->inject(menu);
            }
        }
        return false;
    }

  private:
    InputFormat* m_owner;
};

InputFormat::InputFormat(QObject* parent)
    : QObject(parent)
{
    m_catcher = new Catcher(this);
    m_disarm  = new QTimer(this);
    m_disarm->setSingleShot(true);
    m_disarm->setInterval(0);
    connect(m_disarm, &QTimer::timeout, this, &InputFormat::hop);

    const char* const names[4] = {"&Bold", "&Italic", "U&nderline", "&Spoiler"};
    const int         keys[4]  = {Qt::Key_B, Qt::Key_I, Qt::Key_U, Qt::Key_S};
    for (int i = 0; i < 4; ++i) {
        // The key after a tab only: setShortcut would make TeamSpeak's own Alt+Shift+B/I/U ambiguous.
        m_top[i] = new QAction(i18n::t(names[i]) + QLatin1Char('\t') + altShift(keys[i]).toString(QKeySequence::NativeText), this);
        m_top[i]->setCheckable(true);
        const textformat::Format f = kTop[i];
        connect(m_top[i], &QAction::triggered, this, [this, f] { triggered(f); });
    }
    m_formatSpoiler = new QAction(i18n::t("&Spoiler") + QLatin1Char('\t') + altShift(Qt::Key_S).toString(QKeySequence::NativeText), this);
    connect(m_formatSpoiler, &QAction::triggered, this, [this] { triggered(textformat::Format::Spoiler); });
}

InputFormat::~InputFormat()
{
    shutdown();
}

QAction* InputFormat::action(textformat::Format f) const
{
    for (int i = 0; i < 4; ++i) {
        if (kTop[i] == f)
            return m_top[i];
    }
    return nullptr;
}

void InputFormat::attach(QTextEdit* edit)
{
    if (!edit)
        return;
    m_edits.removeAll(QPointer<QTextEdit>());
    for (const QPointer<QTextEdit>& known : qAsConst(m_edits)) {
        if (known == edit)
            return;
    }
    m_edits.append(edit);
    edit->installEventFilter(this);
    edit->viewport()->installEventFilter(this);
}

void InputFormat::detach(QTextEdit* edit)
{
    if (!edit)
        return;
    edit->removeEventFilter(this);
    edit->viewport()->removeEventFilter(this);
    m_edits.removeAll(QPointer<QTextEdit>(edit));
}

void InputFormat::shutdown()
{
    disarm();
    for (const QPointer<QTextEdit>& edit : qAsConst(m_edits)) {
        if (!edit)
            continue;
        edit->removeEventFilter(this);
        edit->viewport()->removeEventFilter(this);
    }
    m_edits.clear();
}

void InputFormat::arm(QTextEdit* edit)
{
    m_armedFor = edit;
    if (!m_armed) {
        m_armed = true;
        qApp->installEventFilter(m_catcher);
    }
    // Two hops: once TeamSpeak's handler returned, and once what it posted ran too (a menu opened one turn late).
    m_hops = 0;
    m_disarm->start();
}

void InputFormat::hop()
{
    if (m_hops == 0 && m_armed) {
        m_hops = 1;
        m_disarm->start();
        return;
    }
    disarm();
}

void InputFormat::disarm()
{
    m_disarm->stop();
    m_hops = 0;
    if (!m_armed)
        return;
    m_armed = false;
    qApp->removeEventFilter(m_catcher);
}

void InputFormat::inject(QMenu* menu)
{
    disarm();
    auto* edit = qobject_cast<QTextEdit*>(menu->parentWidget());
    if (!edit)
        return;
    const QTextCursor cursor = edit->textCursor();
    m_menuEdit               = edit;
    m_menuStart              = cursor.selectionStart();
    m_menuEnd                = cursor.selectionEnd();
    const bool placeholder   = (showsPlaceholder && showsPlaceholder(edit)) || edit->property("helpTextVisible").toBool();

    QAction* formatAction = nullptr;
    QAction* underline    = nullptr;
    QMenu*   format       = formatMenuOf(menu, &formatAction, &underline);

    // With text selected: the four entries after "Select All" (else before Format, else at the end).
    const QString text = edit->document()->toRawText();
    int           s    = qBound(0, m_menuStart, text.size());
    int           e    = qBound(0, m_menuEnd, text.size());
    while (s < e && text.at(s).isSpace())
        ++s;
    while (e > s && text.at(e - 1).isSpace())
        --e;
    if (!placeholder && s < e) {
        const QList<QAction*> actions = menu->actions();
        QAction*              before  = formatAction;
        for (int i = 0; i < actions.size(); ++i) {
            if (isSelectAll(actions.at(i))) {
                before = actions.value(i + 1, nullptr);
                break;
            }
        }
        QList<QAction*> ours;
        for (int i = 0; i < 4; ++i) {
            m_top[i]->setChecked(textformat::isApplied(text, m_menuStart, m_menuEnd, kTop[i]));
            ours << m_top[i];
        }
        auto* first = new QAction(menu); // the menu's: it goes with the menu
        first->setSeparator(true);
        auto* last = new QAction(menu);
        last->setSeparator(true);
        menu->insertAction(before, first);
        menu->insertActions(before, ours);
        menu->insertAction(before, last);
    }
    // TeamSpeak's Format submenu: Spoiler after Underline (else at its end).
    if (format && !placeholder) {
        QAction*              after = nullptr;
        const QList<QAction*> subs  = format->actions();
        for (int i = 0; underline && i < subs.size(); ++i) {
            if (subs.at(i) == underline) {
                after = subs.value(i + 1, nullptr);
                break;
            }
        }
        format->insertAction(after, m_formatSpoiler);
    }
}

void InputFormat::triggered(textformat::Format f)
{
    QTextEdit* edit = m_menuEdit.data();
    if (!edit)
        return;
    QTextCursor c = edit->textCursor();
    if (!c.hasSelection() && m_menuStart != m_menuEnd) {
        // The selection as it was when the menu opened.
        const int last = qMax(0, edit->document()->characterCount() - 1);
        c.setPosition(qBound(0, m_menuStart, last));
        c.setPosition(qBound(0, m_menuEnd, last), QTextCursor::KeepAnchor);
        edit->setTextCursor(c);
    }
    run(edit, f);
}

bool InputFormat::run(QTextEdit* edit, textformat::Format f)
{
    if (!edit || (showsPlaceholder && showsPlaceholder(edit)) || edit->property("helpTextVisible").toBool())
        return false;
    QTextDocument*         doc  = edit->document();
    const QString          text = doc->toRawText(); // positions as the document has them (no line breaks converted)
    const QTextCursor      now  = edit->textCursor();
    const textformat::Edit ed   = textformat::apply(text, now.selectionStart(), now.selectionEnd(), f);
    if (!ed.valid())
        return false;
    QTextCursor c(doc);
    c.beginEditBlock(); // one Ctrl+Z
    c.setPosition(ed.from);
    c.setPosition(ed.to, QTextCursor::KeepAnchor);
    c.insertText(ed.replacement);
    c.endEditBlock();
    const int last = qMax(0, doc->characterCount() - 1);
    c.setPosition(qBound(0, ed.selStart, last));
    c.setPosition(qBound(0, ed.selEnd, last), QTextCursor::KeepAnchor);
    edit->setTextCursor(c);
    return true;
}

bool InputFormat::eventFilter(QObject* watched, QEvent* event)
{
    QTextEdit* edit = nullptr;
    for (const QPointer<QTextEdit>& known : qAsConst(m_edits)) {
        if (known && (watched == known.data() || watched == known->viewport())) {
            edit = known.data();
            break;
        }
    }
    if (!edit)
        return false;
    switch (event->type()) {
    case QEvent::ContextMenu:
        // The mouse's comes to the viewport, the Menu key's and Shift+F10's to the input.
        arm(edit);
        break;
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress: {
        if (watched != edit)
            break;
        auto*                     ke   = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = ke->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        // Alt+Shift+S, also on other keyboard layouts (the key, not the letter).
        if (mods == (Qt::AltModifier | Qt::ShiftModifier) && (ke->key() == Qt::Key_S || ke->nativeVirtualKey() == 'S')) {
            if (event->type() == QEvent::ShortcutOverride) {
                ke->accept();
                return true;
            }
            run(edit, textformat::Format::Spoiler);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return false;
}
