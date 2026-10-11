#include "chatinput.h"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QScopedValueRollback>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int kTurnMs     = 500; // TeamSpeak's own change of the text after a focus change comes within this
constexpr int kTryMs      = 25;  // the tries for the focus ...
constexpr int kTries      = 40;  // ... about a second of them
constexpr int kFocusWaits = 3;   // focused but the placeholder still in: TeamSpeak's next moments

// The colour the input's text has of its own (its first character's), invalid when it has none.
QColor textColor(const QTextEdit* edit)
{
    QTextDocument* doc = edit ? edit->document() : nullptr;
    if (!doc || doc->isEmpty())
        return {};
    QTextCursor cursor(doc);
    cursor.setPosition(1); // charFormat(): the character before the cursor
    const QTextCharFormat format = cursor.charFormat();
    if (!format.hasProperty(QTextFormat::ForegroundBrush) || format.foreground().style() == Qt::NoBrush)
        return {};
    return format.foreground().color();
}

} // namespace

ChatInputs::ChatInputs(QObject* parent)
    : QObject(parent)
{
    m_timer = new QTimer(this); // a child (CRASH RULE): stops and goes with this
    m_timer->setInterval(kTryMs);
    connect(m_timer, &QTimer::timeout, this, &ChatInputs::tick);
}

ChatInputs::~ChatInputs()
{
    m_timer->stop();
    for (const auto& w : m_watches) {
        if (w->edit) {
            w->edit->removeEventFilter(this);
            disconnect(w->changes);
        }
    }
}

ChatInputs::Watch* ChatInputs::watchFor(const QObject* input) const
{
    if (!input)
        return nullptr;
    for (const auto& w : m_watches) {
        if (w->edit && w->edit.data() == input)
            return w.get();
    }
    return nullptr;
}

void ChatInputs::prune()
{
    m_watches.erase(std::remove_if(m_watches.begin(), m_watches.end(), [](const std::unique_ptr<Watch>& w) { return w->edit.isNull(); }), m_watches.end());
}

void ChatInputs::attach(QTextEdit* input)
{
    if (!input || watchFor(input))
        return;
    prune();
    auto w   = std::make_unique<Watch>();
    w->edit  = input;
    w->known = input->hasFocus(); // focused: what it holds was typed (TeamSpeak took its placeholder out)
    w->changes = connect(input, &QTextEdit::textChanged, this, [this, input] {
        if (Watch* watch = watchFor(input))
            changed(*watch);
    });
    input->installEventFilter(this);
    m_watches.push_back(std::move(w));
}

void ChatInputs::detach(QTextEdit* input)
{
    for (auto it = m_watches.begin(); it != m_watches.end(); ++it) {
        if ((*it)->edit && (*it)->edit.data() == input) {
            input->removeEventFilter(this);
            disconnect((*it)->changes);
            m_watches.erase(it);
            return;
        }
    }
}

// Before the input handles the event: what it holds when a focus change comes (TeamSpeak puts its
// placeholder in or takes it out right then, see changed()).
bool ChatInputs::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::FocusIn && type != QEvent::FocusOut && type != QEvent::KeyPress && type != QEvent::InputMethod)
        return false;
    Watch* w = watchFor(watched);
    if (!w)
        return false;
    if (type == QEvent::KeyPress || type == QEvent::InputMethod) {
        w->turn = Turn::None; // the user edits it now: no change of the text is TeamSpeak's placeholder
        return false;
    }
    if (!w->known) {
        // The best guess so far carries over: no focus before a focus-in, the focus before a focus-out.
        w->shows = type == QEvent::FocusIn && shownAs(*w, true);
        w->known = true;
    }
    w->turn        = type == QEvent::FocusIn ? Turn::In : Turn::Out;
    w->before      = w->edit->toPlainText();
    w->beforeColor = textColor(w->edit);
    w->turnClock.start();
    if (type == QEvent::FocusOut)
        w->emptyOut = w->before.isEmpty();
    if (type == QEvent::FocusIn && !w->pending.isEmpty() && !m_inserting) {
        // Text waiting for the focus: in once TeamSpeak handled this focus-in (and took its placeholder out).
        w->waiting    = true;
        w->tries      = 0;
        w->focusWaits = 0;
        QMetaObject::invokeMethod(this, [this] { tick(); }, Qt::QueuedConnection);
    }
    return false;
}

void ChatInputs::changed(Watch& w)
{
    if (m_inserting || !w.edit)
        return;
    const QString text   = w.edit->toPlainText();
    const bool    recent = w.turn != Turn::None && w.turnClock.isValid() && w.turnClock.elapsed() <= kTurnMs;
    if (recent && w.turn == Turn::Out && w.before.isEmpty() && !text.isEmpty()) {
        learn(w, text, textColor(w.edit)); // TeamSpeak put its placeholder into the empty input
        w.shows = true;
        w.turn  = Turn::Shown; // only this text: what comes in after it (a drop) is the user's
        return;
    }
    if (recent && w.turn == Turn::Shown && text == w.placeholder) {
        learn(w, text, textColor(w.edit)); // the same text again: TeamSpeak giving it its colour
        w.shows = true;
        return;
    }
    if (recent && w.turn == Turn::In && w.emptyOut && !w.before.isEmpty() && text.isEmpty()) {
        learn(w, w.before, w.beforeColor); // TeamSpeak took its placeholder out on the focus-in
        w.shows = false;
        w.turn  = Turn::None;
        return;
    }
    if (w.turn == Turn::Shown)
        w.turn = Turn::None;
    if (w.shows && text != placeholderOf(w))
        w.shows = false;
}

void ChatInputs::learn(Watch& w, const QString& text, const QColor& color)
{
    const QColor normal = w.edit ? w.edit->palette().color(QPalette::Text) : QColor();
    w.placeholder       = text;
    w.color             = color.isValid() && color.rgba() != normal.rgba() ? color : QColor();
    m_placeholder       = w.placeholder;
    m_color             = w.color;
}

QString ChatInputs::placeholderOf(const Watch& w) const
{
    return w.placeholder.isEmpty() ? m_placeholder : w.placeholder;
}

QColor ChatInputs::colorOf(const Watch& w) const
{
    return w.placeholder.isEmpty() ? m_color : w.color;
}

bool ChatInputs::shown(const Watch& w) const
{
    return w.edit && shownAs(w, !w.edit->hasFocus());
}

bool ChatInputs::shownAs(const Watch& w, bool unfocused) const
{
    const QString p = placeholderOf(w);
    if (!w.edit || p.isEmpty() || w.edit->toPlainText() != p)
        return false;
    const QColor own   = colorOf(w);
    const QColor color = textColor(w.edit);
    if (own.isValid() && color.isValid() && color.rgba() == own.rgba())
        return true; // in TeamSpeak's placeholder colour, which typing never gives
    const bool typed = !color.isValid() || color.rgba() == w.edit->palette().color(QPalette::Text).rgba();
    if (own.isValid() && typed)
        return false; // the same words in the colour typing gives: the user's
    return w.known ? w.shows : unfocused;
}

bool ChatInputs::decidable(const Watch& w) const
{
    if (!w.edit)
        return false;
    // Not decidable only for an input never seen changing focus that holds text while no placeholder
    // is known at all (it may well be TeamSpeak's, in a language not seen yet).
    return w.edit->toPlainText().isEmpty() || w.known || !placeholderOf(w).isEmpty();
}

void ChatInputs::insert(QTextEdit* input, const QString& text, bool keepFocus)
{
    if (!input || text.isEmpty())
        return;
    attach(input);
    Watch* w = watchFor(input);
    if (!w)
        return;
    w->pending += text;
    w->tries      = 0;
    w->focusWaits = 0;
    w->activated  = false;
    w->refocused  = false;
    if (keepFocus) {
        // The picker stays open: the input gets the focus just for this (TeamSpeak takes its placeholder
        // out), then the keyboard goes back. What can't go in now waits for the input's next focus-in
        // (the picker closing gives it), not for the timer, which would take the keyboard again.
        const QPointer<QWidget> back = QApplication::focusWidget();
        if (!input->hasFocus() && input->isVisible() && input->isEnabled())
            giveFocus(*w);
        if (input->hasFocus() && unsettled(*w))
            refocus(*w);
        if (input->hasFocus() && !unsettled(*w))
            put(*w);
        w->waiting = false;
        if (back && back.data() != input && input->hasFocus())
            back->setFocus(Qt::PopupFocusReason);
        return;
    }
    attempt(*w);
    if (w->waiting && !m_timer->isActive())
        m_timer->start();
}

// The input gets the focus from us; with the window active the focus-in comes now (else when the window
// is activated), and TeamSpeak takes its placeholder out, during it or a moment later. An input never seen
// changing focus whose text is still what it was before that focus-in may hold TeamSpeak's placeholder in
// a language not learned yet: noted, the text then waits a few tries for a change (unsettled()).
void ChatInputs::giveFocus(Watch& w)
{
    if (!w.known && !w.settling && !w.edit->toPlainText().isEmpty()) {
        w.settling = true;
        w.settle   = w.edit->toPlainText();
    }
    w.edit->setFocus(Qt::OtherFocusReason);
}

// Focused, and TeamSpeak's placeholder (as far as known, or see giveFocus()) still in after the focus-in
// from us: a TeamSpeak that takes it out only on its user's own focus-in gets a click's. Once per insert,
// to the input alone: it has the focus already (no focus moves, nothing else sees a change).
void ChatInputs::refocus(Watch& w)
{
    if (w.refocused || !w.edit)
        return;
    w.refocused = true;
    QFocusEvent in(QEvent::FocusIn, Qt::MouseFocusReason);
    QCoreApplication::sendEvent(w.edit, &in);
}

// Focused, but TeamSpeak's placeholder may still be in: it is (as far as known), or see giveFocus().
bool ChatInputs::unsettled(const Watch& w) const
{
    return shown(w) || (w.settling && w.edit && w.edit->toPlainText() == w.settle);
}

void ChatInputs::attempt(Watch& w)
{
    QTextEdit* edit = w.edit;
    if (!edit || w.pending.isEmpty()) {
        w.pending.clear();
        w.waiting = false;
        return;
    }
    if (!edit->hasFocus() && edit->isVisible() && edit->isEnabled()) {
        QWidget* window = edit->window();
        if (!window->isActiveWindow() && !w.activated) {
            w.activated = true; // once: Windows flashes the taskbar button for each refused request
            if (window->isMinimized())
                window->setWindowState((window->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            window->raise();
            window->activateWindow();
        }
        // With the window still coming to the front the focus-in comes when it is activated.
        giveFocus(w);
    }
    if (edit->hasFocus()) {
        if (unsettled(w)) {
            if (w.focusWaits++ < kFocusWaits) {
                w.waiting = true; // TeamSpeak may take it out a moment later: the text goes in on a later try
                return;
            }
            if (!w.refocused) {
                refocus(w); // still in: a click's focus-in, then its moments again
                if (unsettled(w)) {
                    w.focusWaits = 0;
                    w.waiting    = true;
                    return;
                }
            }
        }
        put(w); // with the placeholder still in, put() takes it out
        return;
    }
    if (++w.tries < kTries) {
        w.waiting = true;
        return;
    }
    // No focus in time (Windows kept another window in front): in without it when its text can be told
    // apart from the placeholder, otherwise with the input's next focus-in.
    w.waiting = false;
    if (decidable(w))
        put(w);
}

void ChatInputs::put(Watch& w)
{
    QTextEdit*    edit = w.edit;
    const QString text = w.pending;
    w.pending.clear();
    w.waiting  = false;
    w.settling = false;
    w.settle.clear();
    if (!edit || text.isEmpty())
        return;
    const bool                 placeholder = shown(w);
    const QScopedValueRollback<bool> inserting(m_inserting, true);
    QTextCursor                cursor = edit->textCursor();
    if (placeholder) {
        // TeamSpeak's placeholder is still in: out with it and its colour, so it can't go out with the text.
        // Outside the undo history: Ctrl+Z would bring it back as text of the input, and Enter send it (the
        // history held nothing but the placeholder's own coming and going).
        QTextDocument* doc  = edit->document();
        const bool     undo = doc->isUndoRedoEnabled();
        doc->setUndoRedoEnabled(false);
        cursor.select(QTextCursor::Document);
        cursor.removeSelectedText();
        cursor.setBlockCharFormat(QTextCharFormat());
        doc->setUndoRedoEnabled(undo);
        cursor.insertText(text, QTextCharFormat());
    } else {
        cursor.insertText(text);
    }
    edit->setTextCursor(cursor);
    edit->ensureCursorVisible();
    w.shows    = false;
    w.turn     = Turn::None;
    w.emptyOut = false; // ours now: should TeamSpeak take it out on a focus-in, it isn't its placeholder
}

void ChatInputs::tick()
{
    bool more = false;
    for (size_t i = 0; i < m_watches.size(); ++i) {
        Watch& w = *m_watches[i];
        if (!w.waiting)
            continue;
        attempt(w);
        more = more || w.waiting;
    }
    if (!more)
        m_timer->stop();
    else if (!m_timer->isActive())
        m_timer->start();
}

QString ChatInputs::typedText(const QTextEdit* input) const
{
    if (!input)
        return {};
    const Watch* w = watchFor(input);
    return w && shown(*w) ? QString() : input->toPlainText();
}

bool ChatInputs::showsPlaceholder(const QTextEdit* input) const
{
    const Watch* w = watchFor(input);
    return w && shown(*w);
}

QString ChatInputs::placeholder(const QTextEdit* input) const
{
    const Watch* w = watchFor(input);
    return w ? placeholderOf(*w) : m_placeholder;
}

bool ChatInputs::hasPending(const QTextEdit* input) const
{
    const Watch* w = watchFor(input);
    return w && !w->pending.isEmpty();
}
