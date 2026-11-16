#include "chatzoom.h"

#include <QChildEvent>
#include <QEvent>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>

#include <cmath>
#include <utility>

#include "chatscroll.h"
#include "settings.h"

namespace chatzoom {

int clampTenths(int tenths)
{
    return qBound(Settings::chatZoomRange.min, tenths, Settings::chatZoomRange.max);
}

qreal targetPoints(qreal basePoints, int tenths)
{
    return qMax(kMinPoints, basePoints + clampTenths(tenths) / 10.0);
}

int percentOf(int tenths, qreal basePoints)
{
    const qreal base = basePoints > 0 ? basePoints : kDefaultBasePoints;
    return qRound(100.0 * targetPoints(base, tenths) / base);
}

QString deltaText(int tenths)
{
    const int t = clampTenths(tenths);
    return (t < 0 ? QString::fromLatin1("-") : QString::fromLatin1("+")) + QString::number(std::abs(t) / 10.0, 'f', 1) + QString::fromLatin1(" pt");
}

QByteArray baseProperty()
{
    return QByteArray("tsmediaZoomBase"); // a copy made at run time (CRASH RULE)
}

QByteArray appliedProperty()
{
    return QByteArray("tsmediaZoomSet");
}

} // namespace chatzoom

namespace {
constexpr int kSyncMs = 300;
constexpr int kSaveMs = 500;
} // namespace

ChatZoom::ChatZoom(Host host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
{
    const auto ownedTimer = [this](int ms) { // a child, single shot (CRASH RULE)
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(ms);
        return t;
    };
    m_wheelEnd = ownedTimer(0);
    connect(m_wheelEnd, &QTimer::timeout, this, [this] { m_wheel = nullptr; });
    m_sync = ownedTimer(kSyncMs);
    connect(m_sync, &QTimer::timeout, this, &ChatZoom::syncVisible);
    m_apply = ownedTimer(0);
    connect(m_apply, &QTimer::timeout, this, &ChatZoom::applyQueued);
    m_save = ownedTimer(kSaveMs);
    connect(m_save, &QTimer::timeout, this, [] { Settings::instance().save(); });
    m_discoverSoon = ownedTimer(0);
    connect(m_discoverSoon, &QTimer::timeout, this, [this] {
        if (!m_down && m_host.discover)
            m_host.discover();
    });
    m_endSoon = ownedTimer(0);
    connect(m_endSoon, &QTimer::timeout, this, &ChatZoom::keepEnds);
}

ChatZoom::~ChatZoom()
{
    shutdown();
}

void ChatZoom::shutdown()
{
    if (m_down)
        return;
    m_down = true;
    m_wheelEnd->stop();
    m_sync->stop();
    m_apply->stop();
    m_discoverSoon->stop();
    m_endSoon->stop();
    m_endQueue.clear();
    if (m_save->isActive()) {
        m_save->stop();
        Settings::instance().save(); // the last wheel's zoom isn't lost
    }
    for (const QPointer<QTextBrowser>& b : qAsConst(m_browsers)) {
        if (!b)
            continue;
        b->removeEventFilter(this);
        b->viewport()->removeEventFilter(this);
    }
    for (const QPointer<QObject>& o : qAsConst(m_tabObjects)) {
        if (o)
            o->removeEventFilter(this);
    }
    m_browsers.clear();
    m_tabObjects.clear();
    m_applyQueue.clear();
    m_wheel = nullptr;
}

int ChatZoom::zoom() const
{
    return chatzoom::clampTenths(Settings::instance().chatZoom);
}

bool ChatZoom::hasBase(QTextBrowser* browser) const
{
    const QByteArray name = chatzoom::baseProperty();
    return browser && browser->property(name.constData()).userType() == QMetaType::QFont;
}

QFont ChatZoom::baseOf(QTextBrowser* browser) const
{
    const QByteArray name = chatzoom::baseProperty();
    const QVariant   v    = browser ? browser->property(name.constData()) : QVariant();
    return v.userType() == QMetaType::QFont ? v.value<QFont>() : (browser ? browser->font() : QFont());
}

void ChatZoom::setBase(QTextBrowser* browser, const QFont& font)
{
    const QByteArray name = chatzoom::baseProperty();
    browser->setProperty(name.constData(), QVariant(font)); // QFont: Qt's own type, no metatype of ours
}

bool ChatZoom::isOurs(QTextBrowser* browser, const QFont& font) const
{
    const QByteArray name = chatzoom::appliedProperty();
    const QVariant   v    = browser ? browser->property(name.constData()) : QVariant();
    return v.userType() == QMetaType::QFont && sameSize(font, v.value<QFont>());
}

qreal ChatZoom::basePoints(QTextBrowser* browser) const
{
    if (!browser)
        return chatzoom::kDefaultBasePoints;
    const qreal pt = baseOf(browser).pointSizeF();
    return pt > 0 ? pt : chatzoom::kDefaultBasePoints;
}

int ChatZoom::percent() const
{
    return chatzoom::percentOf(zoom(), basePoints(m_host.visible ? m_host.visible() : nullptr));
}

QFont ChatZoom::targetFor(const QFont& base, int tenths) const
{
    QFont f = base;
    f.setPointSizeF(chatzoom::targetPoints(base.pointSizeF(), tenths));
    return f;
}

bool ChatZoom::sameSize(const QFont& a, const QFont& b) const
{
    return a.family() == b.family() && std::abs(a.pointSizeF() - b.pointSizeF()) < 0.01;
}

QTextBrowser* ChatZoom::browserOf(QObject* watched) const
{
    for (const QPointer<QTextBrowser>& b : m_browsers) {
        if (b && (b.data() == watched || b->viewport() == watched))
            return b;
    }
    return nullptr;
}

void ChatZoom::attach(QTextBrowser* browser)
{
    if (m_down || !browser)
        return;
    for (const QPointer<QTextBrowser>& b : qAsConst(m_browsers)) {
        if (b == browser)
            return;
    }
    m_browsers.removeAll(QPointer<QTextBrowser>());
    m_browsers.append(browser);
    browser->installEventFilter(this);             // FontChange, Show
    browser->viewport()->installEventFilter(this); // Ctrl + wheel (seen before QTextEdit zooms)
    const QFont font = browser->font();
    // The base: first seen, or TeamSpeak (or a zoom without TS Media) changed the font while we were away. A chat at its base,
    // at the zoom, or at the size we set last (an older zoom it hadn't shown since) keeps its base.
    if (font.pointSizeF() > 0
        && (!hasBase(browser) || (!sameSize(font, targetFor(baseOf(browser), zoom())) && !sameSize(font, baseOf(browser)) && !isOurs(browser, font))))
        setBase(browser, font);
    ensure(browser);
}

void ChatZoom::watchTabs(QWidget* tabWidget)
{
    if (m_down || !tabWidget)
        return;
    for (const QPointer<QObject>& o : qAsConst(m_tabObjects)) {
        if (o == tabWidget)
            return;
    }
    m_tabObjects.removeAll(QPointer<QObject>());
    m_tabObjects.append(tabWidget);
    tabWidget->installEventFilter(this);
    // QTabWidget keeps its pages in a QStackedWidget of its own: a new tab arrives there.
    for (QStackedWidget* stack : tabWidget->findChildren<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        m_tabObjects.append(stack);
        stack->installEventFilter(this);
    }
}

bool ChatZoom::ensure(QTextBrowser* browser)
{
    if (!browser || m_down || m_applying)
        return false;
    const QFont font = browser->font();
    if (font.pointSizeF() <= 0)
        return false; // a pixel-sized font: left alone
    if (!hasBase(browser))
        setBase(browser, font);
    const QFont base = baseOf(browser);
    if (base.pointSizeF() <= 0)
        return false;
    const QFont want = targetFor(base, zoom());
    if (sameSize(font, want))
        return false;
    m_applying = true; // QTextEdit hands the font to its document (the wheel's own path)
    browser->setFont(want);
    m_applying = false;
    const QByteArray set = chatzoom::appliedProperty();
    browser->setProperty(set.constData(), QVariant(want));
    ++m_fontsSet;
    return true;
}

void ChatZoom::setChatZoom(int tenths)
{
    if (m_down)
        return;
    const int t       = chatzoom::clampTenths(tenths);
    const int before  = zoom();
    if (m_host.endLoadHolds)
        m_host.endLoadHolds(); // a font change from outside the chat: the load's place keeping lets go first
    Settings::instance().chatZoom = t;
    m_save->stop();
    Settings::instance().save();
    syncVisible();
    if (t != before)
        emit chatZoomChanged(t);
}

void ChatZoom::syncVisible()
{
    if (m_down)
        return;
    for (const QPointer<QTextBrowser>& b : QVector<QPointer<QTextBrowser>>(m_browsers)) {
        if (b && b->isVisible())
            ensure(b); // hidden ones on their Show
    }
}

void ChatZoom::keepEndSoon(QTextBrowser* browser)
{
    if (m_down)
        return;
    if (!m_endQueue.contains(browser))
        m_endQueue.append(browser);
    m_endSoon->start(); // a posted event: it runs ahead of the paint (QWidget's UpdateRequest has a lower priority)
}

void ChatZoom::keepEnds()
{
    const QVector<QPointer<QTextBrowser>> queue = std::exchange(m_endQueue, {});
    for (const QPointer<QTextBrowser>& b : queue) {
        if (b && !m_down && b->isVisible())
            chatscroll::toBottom(b); // the whole chat laid out once (ChatScale's settle then finds it done)
    }
}

void ChatZoom::queueApply(QTextBrowser* browser)
{
    if (!m_applyQueue.contains(browser))
        m_applyQueue.append(browser);
    m_apply->start();
}

void ChatZoom::applyQueued()
{
    const QVector<QPointer<QTextBrowser>> queue = std::exchange(m_applyQueue, {});
    for (const QPointer<QTextBrowser>& b : queue) {
        if (b)
            ensure(b);
    }
}

void ChatZoom::userZoomed(QTextBrowser* browser)
{
    const QFont font   = browser->font();
    const QFont base   = baseOf(browser);
    const int   before = zoom();
    const int   t      = chatzoom::clampTenths(qRound(10.0 * (font.pointSizeF() - base.pointSizeF())));
    Settings::instance().chatZoom = t;
    m_save->start(); // saved after the last notch
    if (!sameSize(font, targetFor(base, t)))
        queueApply(browser); // past a bound, or a touchpad's fraction: back to the saved size
    m_sync->start();         // the other visible chats follow after the last notch
    if (t != before)
        emit chatZoomChanged(t);
}

void ChatZoom::fontChanged(QTextBrowser* browser)
{
    const QFont font = browser->font();
    if (font.pointSizeF() <= 0)
        return;
    if (m_wheel == browser) {
        userZoomed(browser); // Qt's Ctrl + wheel zoom, inside the wheel event
        return;
    }
    if (sameSize(font, targetFor(baseOf(browser), zoom())) || isOurs(browser, font))
        return; // the size we want (a style's re-polish, our own font)
    // TeamSpeak set a font (Options > Chat OK / Apply, its reset): the chat's new base; the zoom goes back on top a turn
    // later (never a setFont inside the font event).
    setBase(browser, font);
    queueApply(browser);
}

bool ChatZoom::eventFilter(QObject* watched, QEvent* event)
{
    if (m_down)
        return QObject::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::Wheel:
        if (static_cast<QWheelEvent*>(event)->modifiers() & Qt::ControlModifier) {
            if (QTextBrowser* b = browserOf(watched); b && watched == b->viewport()) {
                m_wheel = b;
                m_wheelEnd->start();
            }
        }
        break;
    case QEvent::FontChange:
        if (!m_applying) {
            if (QTextBrowser* b = browserOf(watched); b && watched == b)
                fontChanged(b);
        }
        break;
    case QEvent::Show:
        if (QTextBrowser* b = browserOf(watched); b && watched == b) {
            // Before its first paint. A chat at its end that gets the zoom now (a tab hidden through a zoom change) is
            // put at its end again before that paint too: Qt keeps the scroll value while the chat grows, and ChatScale
            // restores the end only at its 150 ms settle (review fix: the chat jumped down on its first show).
            const bool bottom = chatscroll::atBottom(b);
            if (ensure(b) && bottom)
                keepEndSoon(b);
        }
        break;
    case QEvent::ChildAdded:
        if (static_cast<QChildEvent*>(event)->child() && static_cast<QChildEvent*>(event)->child()->isWidgetType()) {
            for (const QPointer<QObject>& o : qAsConst(m_tabObjects)) {
                if (o == watched) {
                    m_discoverSoon->start(); // a new tab's chat, zoomed before its first paint
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
