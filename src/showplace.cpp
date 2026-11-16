#include "showplace.h"

#include <QEvent>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>

#include <utility>

namespace {
constexpr int kTargetSlack = 2; // px: Qt's own rounding of the cursor's rectangle
} // namespace

ShowPlace::ShowPlace(Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(0);
    connect(m_timer, &QTimer::timeout, this, &ShowPlace::settle);
}

ShowPlace::~ShowPlace()
{
    stop();
}

void ShowPlace::watch(QTextBrowser* browser)
{
    if (m_stopped || !browser)
        return;
    for (const QPointer<QTextBrowser>& b : qAsConst(m_watched)) {
        if (b == browser)
            return;
    }
    m_watched.removeAll(QPointer<QTextBrowser>());
    m_watched.append(browser);
    browser->installEventFilter(this);
}

void ShowPlace::stop()
{
    m_stopped = true;
    m_timer->stop();
    disarmAll();
    m_endAtResize.clear();
    for (const QPointer<QTextBrowser>& b : qAsConst(m_watched)) {
        if (b)
            b->removeEventFilter(this);
    }
    m_watched.clear();
}

bool ShowPlace::eventFilter(QObject* watched, QEvent* event)
{
    if (m_stopped || !watched->isWidgetType())
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::Resize && type != QEvent::Show)
        return false;
    auto* browser = qobject_cast<QTextBrowser*>(watched);
    if (!browser)
        return false;
    if (type == QEvent::Resize) {
        // Qt delivers a hidden chat's pending resize right before its Show and lays the chat out for its new width in it:
        // whether the hidden chat was at its end. Not while dark mode keeps a place for the Show (that place wins).
        if (!browser->isVisible() && browser->document()->characterCount() > 1) {
            const bool waits = m_hooks.placeWaits && m_hooks.placeWaits(browser);
            m_endAtResize.insert(browser, !waits && chatscroll::atBottom(browser));
            m_timer->start(); // forgotten next turn if no Show follows (dark mode's settle at its shutdown sends one too)
        }
        return false;
    }
    shown(browser);
    return false;
}

void ShowPlace::shown(QTextBrowser* browser)
{
    const bool endAtResize = m_endAtResize.take(browser);
    disarm(browser); // a second Show in the same turn: armed again below
    if (browser->document()->characterCount() <= 1)
        return;
    QScrollBar*                    bar  = browser->verticalScrollBar();
    const chatscroll::ReaderPlace* held = m_hooks.heldPlace ? m_hooks.heldPlace(browser) : nullptr;
    chatscroll::ReaderPlace        place;
    if (held)
        place = *held;
    else if (endAtResize || chatscroll::atBottom(browser))
        place.bottom = true;
    else
        place = chatscroll::captureReader(browser);
    // The place before Qt's move: a load hold's place, or the end the hidden chat had before its new width (a narrower
    // chat grows taller under the same scroll value).
    if (held || (place.bottom && !chatscroll::atBottom(browser)))
        chatscroll::restoreReader(browser, place);
    // Review fix: a scale pass the saved zoom started at this Show (ChatZoom's filter runs before this one) read the place
    // as the new width had left it, short of the end; it keeps this place instead. Only a place known from before that
    // font: the load hold's, or the end read at the hidden resize (the end read now may be a reader's place the new font
    // pushed down to the end, which the pass read right).
    const bool decided = held || endAtResize;
    if (decided && m_hooks.placed)
        m_hooks.placed(browser);
    // Where Qt's first-show move would take the view (nothing when the cursor's line shows already). Armed for exactly
    // that move: the first value change to it, inside QTextEdit's showEvent, puts the place back at once, before anything
    // paints. A Show that isn't the chat's first moves nothing, and the watch goes next turn.
    const int target = chatscroll::cursorJumpTarget(browser);
    const int before = bar->value();
    if (target == before)
        return;
    ++m_arms;
    Armed armed;
    armed.browser = browser;
    const QPointer<QTextBrowser> guard(browser);
    armed.watch = connect(bar, &QScrollBar::valueChanged, this, [this, guard, place, decided, target, before](int value) {
        if (!guard || qAbs(value - target) > kTargetSlack)
            return;
        disarm(guard);
        QTextBrowser* b = guard;
        if (!place.bottom && !place.cursor.isNull() && place.cursor.document() != b->document())
            return; // TeamSpeak gave the chat another document meanwhile
        ++m_fixes;
        chatscroll::restoreReader(b, place);
        if (decided && m_hooks.placed)
            m_hooks.placed(b);
        if (m_hooks.putBack)
            m_hooks.putBack(b, before, value, b->verticalScrollBar()->value());
    });
    m_armed.append(armed);
    m_timer->start();
}

void ShowPlace::disarm(QTextBrowser* browser)
{
    for (int i = m_armed.size() - 1; i >= 0; --i) {
        if (m_armed.at(i).browser == browser || !m_armed.at(i).browser) {
            QObject::disconnect(m_armed.at(i).watch);
            m_armed.removeAt(i);
        }
    }
}

void ShowPlace::disarmAll()
{
    for (const Armed& a : qAsConst(m_armed))
        QObject::disconnect(a.watch);
    m_armed.clear();
}

void ShowPlace::settle()
{
    m_endAtResize.clear();
    disarmAll();
}
