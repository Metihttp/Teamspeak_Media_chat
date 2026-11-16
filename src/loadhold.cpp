#include "loadhold.h"

#include <QEvent>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>

#include <utility>

LoadHold::LoadHold(QTextBrowser* browser, const chatscroll::ReaderPlace& place, std::function<bool()> busy, QObject* parent,
                   int quietMs, int capMs)
    : QObject(parent)
    , m_browser(browser)
    , m_doc(browser ? browser->document() : nullptr)
    , m_viewport(browser ? browser->viewport() : nullptr)
    , m_place(place)
    , m_busy(std::move(busy))
{
    m_age.start();
    m_apply = new QTimer(this);
    m_apply->setSingleShot(true);
    m_apply->setInterval(0);
    connect(m_apply, &QTimer::timeout, this, &LoadHold::apply);
    m_quiet = new QTimer(this);
    m_quiet->setSingleShot(true);
    m_quiet->setInterval(qMax(0, quietMs));
    connect(m_quiet, &QTimer::timeout, this, &LoadHold::quiet);
    m_cap = new QTimer(this);
    m_cap->setSingleShot(true);
    m_cap->setInterval(qMax(0, capMs));
    connect(m_cap, &QTimer::timeout, this, [this] { finish(End::Cap); });
    if (!browser || !m_doc) {
        m_finished = true;
        return;
    }
    QScrollBar* bar = browser->verticalScrollBar();
    // Not valueChanged: every module sets the value in the turn of its own edit, which already scheduled the apply.
    m_range    = connect(bar, &QScrollBar::rangeChanged, this, [this] { changed(); });
    m_contents = connect(m_doc.data(), &QTextDocument::contentsChange, this, [this] { changed(); });
    // Wheel, keys, a drag or a click on the bar: Qt emits it before the value changes.
    m_action = connect(bar, &QScrollBar::actionTriggered, this, [this] { finish(End::User); });
    // Installed last (after the modules' filters): seen first, before a module may take the event.
    browser->installEventFilter(this);
    browser->viewport()->installEventFilter(this);
    m_quiet->start();
    m_cap->start();
}

LoadHold::~LoadHold()
{
    finish(End::None);
}

void LoadHold::finish(End why)
{
    if (m_finished)
        return;
    m_finished = true;
    m_end      = why;
    m_apply->stop();
    m_quiet->stop();
    m_cap->stop();
    disconnect(m_range);
    disconnect(m_contents);
    disconnect(m_action);
    if (m_browser)
        m_browser->removeEventFilter(this);
    if (m_viewport)
        m_viewport->removeEventFilter(this);
    if (onFinished) {
        const std::function<void(End)> done = std::move(onFinished);
        onFinished                          = nullptr;
        done(why);
    }
}

bool LoadHold::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_finished && (watched == m_browser || watched == m_viewport)) {
        switch (event->type()) {
        case QEvent::Wheel: // also Ctrl + wheel (the zoom keeps its own place from there)
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::KeyPress:
        case QEvent::TouchBegin:
        case QEvent::ContextMenu:
            finish(End::User);
            break;
        default:
            break;
        }
    }
    return false;
}

void LoadHold::changed()
{
    if (m_finished || m_applying)
        return;
    m_apply->start();
    m_quiet->start();
}

void LoadHold::apply()
{
    if (m_finished)
        return;
    if (!m_browser || !m_doc) {
        finish(End::Swapped);
        return;
    }
    QTextDocument* doc = m_browser->document();
    if (doc != m_doc || (!m_place.cursor.isNull() && m_place.cursor.document() != doc)) {
        finish(End::Swapped);
        return;
    }
    QScrollBar* bar  = m_browser->verticalScrollBar();
    int         want = bar->maximum();
    if (!m_place.bottom) {
        const qreal y = chatscroll::readerTarget(doc, m_place); // a folded message: the row it folded into
        if (y < 0)
            return;
        want = qBound(bar->minimum(), qRound(y), bar->maximum());
    }
    if (bar->value() == want)
        return;
    m_applying = true;
    bar->setValue(want);
    m_applying = false;
    ++m_corrections;
}

void LoadHold::quiet()
{
    if (m_finished)
        return;
    if (m_busy && m_busy()) {
        m_quiet->start();
        return;
    }
    finish(End::Settled);
}
