#include "attachpopup.h"

#include <QAbstractButton>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include "i18n.h"
#include "uiutil.h"

namespace {

constexpr int kPadding  = 6;  // inside the panel, around the items
constexpr int kItemH    = 42;
constexpr int kTile     = 32;
constexpr int kGlyph    = 22;
constexpr int kMinWidth = 216;
constexpr int kRadius   = 12;

QPointer<AttachPopup>& currentPopup()
{
    static QPointer<AttachPopup> popup;
    return popup;
}

struct Look {
    QColor panel;
    QColor border;
    QColor text;
    QColor disabled;
    QColor hover;
    QColor pressed;
    QColor focus;
};

Look lookFor(bool dark, const QColor& base)
{
    Look l;
    if (dark) {
        l.panel = QColor(0x2b, 0x2d, 0x31);
        if (base.isValid() && base.lightness() < 128)
            l.panel = ui::flatten(QColor(255, 255, 255, 9), base); // a step above the chat
        l.border   = QColor(255, 255, 255, 22);
        l.text     = QColor(0xe3, 0xe5, 0xe8);
        l.disabled = QColor(0x80, 0x84, 0x8e);
        l.hover    = QColor(255, 255, 255, 18);
        l.pressed  = QColor(255, 255, 255, 30);
        l.focus    = QColor(0x94, 0x9c, 0xf7);
    } else {
        l.panel    = QColor(0xff, 0xff, 0xff);
        l.border   = QColor(0, 0, 0, 28);
        l.text     = QColor(0x2e, 0x33, 0x38);
        l.disabled = QColor(0x8a, 0x8e, 0x96);
        l.hover    = QColor(0x74, 0x7f, 0x8d, 30);
        l.pressed  = QColor(0x74, 0x7f, 0x8d, 50);
        l.focus    = QColor(0x47, 0x52, 0xc4);
    }
    return l;
}

// The mean colour of an icon's opaque pixels: its tile is tinted with it (any entry's icon, ours or not).
QColor meanColor(const QPixmap& pm)
{
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    double r = 0, g = 0, b = 0;
    int    n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) < 160)
                continue;
            r += qRed(line[x]);
            g += qGreen(line[x]);
            b += qBlue(line[x]);
            ++n;
        }
    }
    if (n == 0)
        return QColor(0x80, 0x84, 0x8e);
    return QColor(qRound(r / n), qRound(g / n), qRound(b / n));
}

} // namespace

// One entry: its tile, its icon and its text. A button, so screen readers and the keyboard see it.
class AttachItem : public QAbstractButton
{
  public:
    AttachItem(AttachPopup* popup, int index, const attachmenu::Entry& entry, const Look& look)
        : QAbstractButton(popup)
        , m_popup(popup)
        , m_index(index)
        , m_look(look)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        const QString text = entry.text ? entry.text() : entry.id;
        setText(text);
        setAccessibleName(text);
        if (entry.icon)
            m_icon = entry.icon();
        setEnabled(!entry.enabled || entry.enabled());
        if (!isEnabled()) {
            // 2.3 ux review: why it is grey (every entry is off for the same reason: no server, no channel).
            const QString why = i18n::t("Connect to a server and join a channel first.");
            setToolTip(why);
            setAccessibleDescription(why);
        }
    }

    QSize sizeHint() const override
    {
        const int textWidth = fontMetrics().horizontalAdvance(text());
        return QSize(qMax(kMinWidth - 2 * kPadding, 10 + kTile + 12 + textWidth + 18), kItemH);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool current = m_popup->currentIndex() == m_index;
        if (isEnabled() && (isDown() || underMouse() || (current && m_popup->keyboardUsed()))) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? m_look.pressed : m_look.hover);
            p.drawRoundedRect(r, 8, 8);
        }
        if (current && m_popup->keyboardUsed()) {
            p.setPen(QPen(m_look.focus, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
        }
        // The tile: the icon's own colour, soft.
        const QRectF tile(10, (height() - kTile) / 2.0, kTile, kTile);
        const qreal  dpr = devicePixelRatioF();
        QPixmap      pm;
        if (!m_icon.isNull()) {
            QWindow* window = this->window() ? this->window()->windowHandle() : nullptr;
            pm = window ? m_icon.pixmap(window, QSize(kGlyph, kGlyph)) : m_icon.pixmap(QSize(kGlyph, kGlyph) * dpr);
        }
        const QColor mean = pm.isNull() ? m_look.disabled : meanColor(pm);
        QColor       tint = mean;
        const bool   dark = m_look.panel.lightness() < 128;
        tint.setAlpha(dark ? 52 : 34);
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawRoundedRect(tile, 9, 9);
        if (dark && !pm.isNull()) {
            // 2.3: on a dark menu the glyph in its own hue, lighter, at 3:1 on its tile (the icons are drawn for
            // light menus: 3.7:1 on white, under 3:1 on their dark tiles).
            const QColor tileColor = ui::flatten(tint, m_look.panel);
            const QColor ink       = ui::readableOn(mean, {tileColor}, 3.2);
            if (ink != mean) {
                QPixmap lit(pm.size());
                lit.setDevicePixelRatio(pm.devicePixelRatio());
                lit.fill(Qt::transparent);
                QPainter q(&lit);
                q.drawPixmap(0, 0, pm);
                q.setCompositionMode(QPainter::CompositionMode_SourceIn);
                q.fillRect(QRect(QPoint(0, 0), pm.size()), ink);
                q.end();
                pm = lit;
            }
        }
        if (!pm.isNull()) {
            const QSizeF logical = QSizeF(pm.size()) / qMax<qreal>(1.0, pm.devicePixelRatioF());
            const QSizeF shown   = logical.width() > kGlyph ? QSizeF(kGlyph, kGlyph) : logical;
            const QRectF at(tile.center().x() - shown.width() / 2.0, tile.center().y() - shown.height() / 2.0, shown.width(), shown.height());
            if (!isEnabled())
                p.setOpacity(0.45);
            p.drawPixmap(at, pm, QRectF(QPointF(0, 0), QSizeF(pm.size())));
            p.setOpacity(1.0);
        }
        p.setPen(isEnabled() ? m_look.text : m_look.disabled);
        const QRect textRect(qRound(tile.right()) + 12, 0, width() - qRound(tile.right()) - 12 - 8, height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width()));
    }

    void keyPressEvent(QKeyEvent* event) override { event->ignore(); } // the popup handles every key
    void focusInEvent(QFocusEvent* event) override
    {
        QAbstractButton::focusInEvent(event);
        m_popup->setCurrent(m_index);
    }
    void enterEvent(QEvent* event) override
    {
        QAbstractButton::enterEvent(event);
        if (isEnabled())
            m_popup->setCurrent(m_index); // the pointer and the keys go on from the same item
    }

  private:
    AttachPopup* m_popup;
    int          m_index;
    Look         m_look;
    QIcon        m_icon;
};

AttachPopup::AttachPopup(const QList<attachmenu::Entry>& entries, bool dark, const QColor& base, QWidget* anchor)
    : QWidget(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_entries(entries)
    , m_anchor(anchor)
    , m_dark(dark)
    , m_base(base)
{
    setObjectName(QString::fromLatin1("tsmediaAttachMenu")); // swept at plugin shutdown with our other windows
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setLayoutDirection(Qt::LeftToRight);
    setAccessibleName(i18n::t("Attach"));
    if (currentPopup() && currentPopup() != this)
        currentPopup()->close();
    currentPopup() = this;

    const Look look = lookFor(dark, base);
    int        width = kMinWidth - 2 * kPadding;
    for (const attachmenu::Entry& entry : qAsConst(m_entries)) {
        auto* item = new AttachItem(this, m_items.size(), entry, look);
        width      = qMax(width, item->sizeHint().width());
        m_items.append(item);
    }
    for (int i = 0; i < m_items.size(); ++i) {
        m_items.at(i)->setGeometry(kShadow + kPadding, kShadow + kPadding + i * kItemH, width, kItemH);
        connect(m_items.at(i), &QAbstractButton::clicked, this, [this, i] { choose(i); });
    }
    setFixedSize(2 * (kShadow + kPadding) + width, 2 * (kShadow + kPadding) + qMax(1, m_items.size()) * kItemH);
}

AttachPopup::~AttachPopup()
{
    if (currentPopup() == this)
        currentPopup() = nullptr;
}

AttachPopup* AttachPopup::current()
{
    return currentPopup().data();
}

void AttachPopup::openAt(const QRect& opener)
{
    m_opener         = opener;
    const QSize size = this->size();
    // The panel's left edge at the opener's, its bottom 6 px above it (the shadow margin is transparent).
    QPoint   at(opener.left() - kShadow - 4, opener.top() - 6 - size.height() + kShadow);
    QScreen* screen = QGuiApplication::screenAt(opener.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (at.y() < area.top())
            at.setY(opener.bottom() + 6 - kShadow); // no room above: below it
        at.setX(qBound(area.left(), at.x(), area.right() - size.width() + 1));
        at.setY(qBound(area.top(), at.y(), area.bottom() - size.height() + 1));
    }
    move(at);
    show();
    raise();
    activateWindow();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i)->isEnabled()) {
            setCurrent(i);
            m_items.at(i)->setFocus(Qt::PopupFocusReason);
            break;
        }
    }
}

void AttachPopup::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Look   look  = lookFor(m_dark, m_base);
    const QRectF panel = QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);
    // A soft shadow, a little lower than the panel: rings of falling alpha.
    for (int i = kShadow; i >= 1; --i) {
        const qreal t     = static_cast<qreal>(i) / kShadow;
        const int   alpha = qRound((m_dark ? 34.0 : 20.0) * (1.0 - t) * (1.0 - t));
        if (alpha <= 0)
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(panel.adjusted(-i, -i + 2, i, i + 2), kRadius + i, kRadius + i);
    }
    p.setPen(QPen(look.border, 1.0));
    p.setBrush(look.panel);
    p.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
}

void AttachPopup::mousePressEvent(QMouseEvent* event)
{
    // A press outside closes the popup and Qt hands it on; on the paperclip that would open it again at
    // once: a second click on the paperclip closes it instead.
    if (!rect().contains(event->pos()) && m_opener.contains(event->globalPos()))
        setAttribute(Qt::WA_NoMouseReplay);
    QWidget::mousePressEvent(event);
}

void AttachPopup::markKeyboard()
{
    if (m_keyboard)
        return;
    m_keyboard = true;
    for (QAbstractButton* item : qAsConst(m_items))
        item->update(); // the focus ring appears with the first key
}

void AttachPopup::setCurrent(int index)
{
    if (index == m_current || index < 0 || index >= m_items.size())
        return;
    const int old = m_current;
    m_current     = index;
    if (old >= 0 && old < m_items.size())
        m_items.at(old)->update();
    m_items.at(index)->update();
}

void AttachPopup::moveFocus(int from, int step)
{
    const int n = m_items.size();
    for (int k = 1; k <= n; ++k) {
        const int index = ((from + step * k) % n + n) % n;
        if (m_items.at(index)->isEnabled()) {
            setCurrent(index);
            m_items.at(index)->setFocus(Qt::TabFocusReason);
            return;
        }
    }
}

void AttachPopup::keyPressEvent(QKeyEvent* event)
{
    const int current = m_current;
    // 2.3 live test: an entry nothing marks yet (the menu opened with a click, the pointer elsewhere) is
    // where the keys start: the first Down marks it instead of skipping it, as in Windows' own menus.
    const bool marked = m_keyboard || (current >= 0 && m_items.at(current)->underMouse());
    markKeyboard();
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Up:
    case Qt::Key_Backtab:
        if (!m_items.isEmpty())
            moveFocus(current < 0 ? 0 : current, -1);
        return;
    case Qt::Key_Down:
    case Qt::Key_Tab:
        if (!m_items.isEmpty() && !marked && current >= 0 && m_items.at(current)->isEnabled())
            m_items.at(current)->setFocus(Qt::TabFocusReason); // its ring shows now
        else if (!m_items.isEmpty())
            moveFocus(current < 0 ? -1 : current, 1);
        return;
    case Qt::Key_Home:
        if (!m_items.isEmpty())
            moveFocus(-1, 1);
        return;
    case Qt::Key_End:
        if (!m_items.isEmpty())
            moveFocus(m_items.size(), -1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (current >= 0 && m_items.at(current)->isEnabled())
            choose(current);
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void AttachPopup::choose(int index)
{
    if (index < 0 || index >= m_entries.size() || !m_items.at(index)->isEnabled())
        return;
    const attachmenu::Entry entry = m_entries.at(index);
    QPointer<QWidget>       anchor = m_anchor;
    emit picked(entry.id);
    close(); // first: a file chooser or a window may open now
    if (!entry.trigger)
        return;
    if (anchor) {
        // After the popup is gone (a zero delay is a posted event: it dies with the anchor).
        QTimer::singleShot(0, anchor.data(), [entry, anchor] {
            if (anchor)
                entry.trigger(anchor.data());
        });
    } else {
        entry.trigger(nullptr);
    }
}
