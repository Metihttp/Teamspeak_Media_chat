#include "repliespopup.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

#include <cmath>

#include "i18n.h"
#include "replyart.h"
#include "uiutil.h"

namespace {

constexpr int kWidth      = 340;
constexpr int kPadding    = 6;
constexpr int kMaxVisible = 6;

} // namespace

RepliesPopup::RepliesPopup(bool dark, const QColor& base, const QFont& chatFont, const QString& title, const QVector<ReplyRow>& rows, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
    , m_dark(dark)
    , m_base(base)
    , m_title(title)
    , m_rows(rows.mid(qMax(0, rows.size() - kMaxRows)))
    , m_offset(qMax(0, rows.size() - kMaxRows))
    , m_font(replyart::scaledFont(chatFont, 1.0))
{
    setObjectName(QString::fromLatin1("tsmediaRepliesPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setLayoutDirection(Qt::LeftToRight);
    setAccessibleName(title);
    setFont(m_font);
    const QFontMetricsF fm(m_font);
    m_rowHeight = qMax(40, static_cast<int>(std::ceil(fm.height() * 2 + 14)));
    m_header    = qMax(28, static_cast<int>(std::ceil(fm.height() + 14)));
    m_current   = 0;
    setFixedSize(kWidth, 2 * kPadding + m_header + visibleRows() * m_rowHeight);
}

int RepliesPopup::visibleRows() const
{
    return qMax(1, qMin(kMaxVisible, m_rows.size()));
}

void RepliesPopup::openAt(const QPoint& pos)
{
    QPoint   at     = pos;
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (at.y() + height() > area.bottom())
            at.setY(pos.y() - height());
        at.setX(qBound(area.left(), at.x(), area.right() - width() + 1));
        at.setY(qBound(area.top(), at.y(), area.bottom() - height() + 1));
    }
    move(at);
    show();
    raise();
    activateWindow();
    setFocus(Qt::PopupFocusReason);
}

QRectF RepliesPopup::rowRect(int row) const
{
    return QRectF(kPadding, kPadding + m_header + (row - m_first) * m_rowHeight, width() - 2 * kPadding, m_rowHeight);
}

int RepliesPopup::rowAt(const QPoint& pos) const
{
    for (int row = m_first; row < m_rows.size() && row < m_first + visibleRows(); ++row) {
        if (rowRect(row).contains(pos))
            return row;
    }
    return -1;
}

void RepliesPopup::scrollTo(int row)
{
    if (row < m_first)
        m_first = row;
    else if (row >= m_first + visibleRows())
        m_first = row - visibleRows() + 1;
    m_first = qBound(0, m_first, qMax(0, m_rows.size() - visibleRows()));
}

void RepliesPopup::setCurrent(int row)
{
    if (m_rows.isEmpty())
        return;
    m_current = qBound(0, row, m_rows.size() - 1);
    scrollTo(m_current);
    update();
}

void RepliesPopup::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const replyart::Palette pal = replyart::paletteFor(m_dark, m_base);
    QColor background = m_dark ? QColor(0x2b, 0x2d, 0x31) : QColor(0xff, 0xff, 0xff);
    QColor border     = m_dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0xe3, 0xe5, 0xe8);
    if (m_dark && m_base.isValid() && m_base.lightness() < 128)
        background = ui::flatten(QColor(255, 255, 255, 8), m_base); // a step above the chat (like the reaction picker)
    p.setPen(QPen(border, 1.0));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

    const replyart::Palette on    = replyart::paletteFor(m_dark, background);
    const QColor            muted = on.muted;
    QFont                   bold(m_font);
    bold.setWeight(QFont::DemiBold);
    const QFontMetricsF fm(m_font), bm(bold);

    // Header: "3 replies"
    const QRectF head(kPadding + 8, kPadding, width() - 2 * kPadding - 16, m_header);
    replyart::drawGlyph(p, replyart::Glyph::Replies, QRectF(head.left(), head.center().y() - 8, 16, 16), muted);
    p.setFont(bold);
    p.setPen(on.text);
    p.drawText(head.adjusted(22, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, bm.elidedText(m_title, Qt::ElideRight, head.width() - 22));

    p.save();
    p.setClipRect(QRectF(kPadding, kPadding + m_header, width() - 2 * kPadding, visibleRows() * m_rowHeight));
    for (int row = m_first; row < m_rows.size() && row < m_first + visibleRows(); ++row) {
        const ReplyRow& r    = m_rows.at(row);
        const QRectF    box  = rowRect(row);
        const bool      lit  = row == m_hover || (m_keyboard && row == m_current);
        if (lit || row == m_pressed) {
            p.setPen(Qt::NoPen);
            p.setBrush(row == m_pressed ? pal.hover.darker(m_dark ? 90 : 104) : on.hover);
            p.drawRoundedRect(box.adjusted(0, 1, 0, -1), 6, 6);
        }
        if (m_keyboard && row == m_current) {
            p.setPen(QPen(on.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box.adjusted(1, 2, -1, -2), 5, 5);
        }
        const qreal x     = box.left() + 10;
        const qreal right = box.right() - 10;
        const qreal line1 = box.top() + 7 + fm.ascent();
        const qreal line2 = line1 + fm.height() + 2;
        // Name and time
        const QString time  = r.time;
        const qreal   timeW = time.isEmpty() ? 0.0 : fm.horizontalAdvance(time) + 8;
        p.setFont(bold);
        p.setPen(replyart::readable(r.nickColor, background, on.name));
        const QString name = bm.elidedText(r.nick, Qt::ElideRight, qMax(20.0, right - x - timeW));
        replyart::drawRun(p, x, line1, bm, name);
        if (!time.isEmpty()) {
            p.setFont(m_font);
            p.setPen(muted);
            p.drawText(QPointF(x + bm.horizontalAdvance(name) + 8, line1), time);
        }
        // Text or file
        p.setFont(m_font);
        p.setPen(on.text);
        qreal tx = x;
        if (r.media) {
            replyart::drawGlyph(p, replyart::Glyph::Clip, QRectF(tx, line2 - fm.ascent() + (fm.ascent() - 12) / 2.0 + 1, 12, 12), muted);
            tx += 15;
        }
        const QString text = r.text.isEmpty() ? QString() : fm.elidedText(r.text, Qt::ElideRight, right - tx);
        replyart::drawRun(p, tx, line2, fm, text);
    }
    p.restore();

    // More than fit: a thin indicator of where the list is.
    if (m_rows.size() > visibleRows()) {
        const qreal top    = kPadding + m_header;
        const qreal span   = visibleRows() * m_rowHeight;
        const qreal thumbH = qMax(18.0, span * visibleRows() / m_rows.size());
        const qreal thumbY = top + (span - thumbH) * m_first / qMax(1, m_rows.size() - visibleRows());
        p.setPen(Qt::NoPen);
        p.setBrush(muted.lighter(m_dark ? 70 : 150));
        p.drawRoundedRect(QRectF(width() - kPadding - 3, thumbY, 3, thumbH), 1.5, 1.5);
    }
}

void RepliesPopup::mouseMoveEvent(QMouseEvent* event)
{
    const int row = rowAt(event->pos());
    if (row != m_hover) {
        m_hover = row;
        setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void RepliesPopup::mousePressEvent(QMouseEvent* event)
{
    if (!rect().contains(event->pos())) {
        close(); // a click outside a popup closes it
        return;
    }
    m_pressed = event->button() == Qt::LeftButton ? rowAt(event->pos()) : -1;
    update();
}

void RepliesPopup::mouseReleaseEvent(QMouseEvent* event)
{
    const int pressed = m_pressed;
    m_pressed         = -1;
    update();
    if (event->button() == Qt::LeftButton && pressed >= 0 && rowAt(event->pos()) == pressed) {
        emit activated(pressed + m_offset);
        close();
    }
}

void RepliesPopup::leaveEvent(QEvent* event)
{
    m_hover = -1;
    update();
    QWidget::leaveEvent(event);
}

void RepliesPopup::wheelEvent(QWheelEvent* event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0 && m_rows.size() > visibleRows()) {
        m_first = qBound(0, m_first - steps, m_rows.size() - visibleRows());
        update();
    }
    event->accept();
}

void RepliesPopup::keyPressEvent(QKeyEvent* event)
{
    const bool first = !m_keyboard;
    m_keyboard       = true;
    switch (event->key()) {
    case Qt::Key_Up:
        setCurrent(first ? m_current : m_current - 1);
        return;
    case Qt::Key_Down:
        setCurrent(first ? m_current : m_current + 1);
        return;
    case Qt::Key_Home:
        setCurrent(0);
        return;
    case Qt::Key_End:
        setCurrent(m_rows.size() - 1);
        return;
    case Qt::Key_PageUp:
        setCurrent(m_current - visibleRows());
        return;
    case Qt::Key_PageDown:
        setCurrent(m_current + visibleRows());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (m_current >= 0 && m_current < m_rows.size()) {
            emit activated(m_current + m_offset);
            close();
        }
        return;
    case Qt::Key_Escape:
        close();
        return;
    default:
        break;
    }
    if (first)
        update();
    QWidget::keyPressEvent(event);
}
