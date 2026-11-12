#include "gifpanel.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include "i18n.h"
#include "medialink.h"
#include "settings.h"
#include "uiutil.h"
#include "video/mfvideo.h"

namespace {

constexpr int kBar          = 8; // scroll bar, as the emoji grid's
constexpr int kDwellMs      = 150;
constexpr int kSlowDwellMs  = 600;
constexpr int kMinFrameMs   = 20; // at most 50 frames per second
constexpr int kMaxLoopW     = 1280;
constexpr int kMaxLoopH     = 720;
constexpr qint64 kMaxLoopMs = 60000;

int g_animationsForTests = -1;

bool animationsOn()
{
    return g_animationsForTests >= 0 ? g_animationsForTests != 0 : ui::animationsEnabled();
}

QFont scaled(const QFont& base, qreal px, int weight)
{
    QFont f = base;
    f.setPixelSize(qMax(8, qRound(px)));
    f.setWeight(weight);
    return f;
}

QString dot()
{
    return QString::fromUtf8(" \xc2\xb7 ");
}

// The upload arrow of the chat's file cards (chatintegration.cpp drawUploadGlyph), in white on the badge.
void drawUploadGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.setPen(QPen(color, qMax<qreal>(1.2, s * 0.12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(cx, box.top() + s * 0.12), QPointF(cx, box.top() + s * 0.64));
    QPainterPath head;
    head.moveTo(cx - s * 0.24, box.top() + s * 0.36);
    head.lineTo(cx, box.top() + s * 0.12);
    head.lineTo(cx + s * 0.24, box.top() + s * 0.36);
    p.drawPath(head);
    QPainterPath tray;
    tray.moveTo(box.left() + s * 0.14, box.top() + s * 0.62);
    tray.lineTo(box.left() + s * 0.14, box.top() + s * 0.88);
    tray.lineTo(box.right() - s * 0.14, box.top() + s * 0.88);
    tray.lineTo(box.right() - s * 0.14, box.top() + s * 0.62);
    p.drawPath(tray);
}

// The part of a frame that covers box (the aspect kept, centred).
QRectF coverSource(const QSizeF& image, const QSizeF& box)
{
    if (image.isEmpty() || box.isEmpty())
        return {};
    const qreal s = qMax(box.width() / image.width(), box.height() / image.height());
    const QSizeF shown(box.width() / s, box.height() / s);
    return QRectF((image.width() - shown.width()) / 2, (image.height() - shown.height()) / 2, shown.width(), shown.height());
}

} // namespace

// ============================================================================================
// The grid: tiles of the snapshot, scrolled by itself (no QScrollArea: TeamSpeak's style sheets stay out)
// ============================================================================================

class GifGrid : public QWidget
{
  public:
    explicit GifGrid(GifPanel* panel)
        : QWidget(panel)
        , m_panel(panel)
    {
        setObjectName(QString::fromLatin1("tsmediaGifGrid"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(i18n::t("GIFs"));
        setAccessibleDescription(i18n::t("Arrow keys move, Enter sends, Shift+F10 opens the menu."));
    }

    int scroll() const { return m_scroll; }

    QRect contentTile(int index) const
    {
        const int row = index / GifPanel::kColumns;
        const int col = index % GifPanel::kColumns;
        return QRect(col * GifPanel::kCellW + 2, row * GifPanel::kCellH + 4, GifPanel::kTileW, GifPanel::kTileH);
    }
    QRect tile(int index) const { return contentTile(index).translated(0, -m_scroll); }

    int contentHeight() const
    {
        const int rows = (m_panel->m_order.size() + GifPanel::kColumns - 1) / GifPanel::kColumns;
        return rows * GifPanel::kCellH + 4;
    }

    int indexAt(const QPoint& pos) const
    {
        if (pos.x() >= width() - kBar - 2 && contentHeight() > height())
            return -1;
        for (int i = 0; i < m_panel->m_order.size(); ++i) {
            if (tile(i).contains(pos))
                return i;
        }
        return -1;
    }
    QString idAt(const QPoint& pos) const { return m_panel->m_order.value(indexAt(pos)); }

    void setScroll(int value)
    {
        const int bounded = qBound(0, value, qMax(0, contentHeight() - height()));
        if (bounded == m_scroll)
            return;
        m_scroll = bounded;
        m_panel->stopAnimation(); // a scroll stops it; the rest starts it again
        m_panel->m_target.clear();
        update();
    }

    void ensureVisible(int index)
    {
        const QRect r = contentTile(index);
        if (r.top() - 4 < m_scroll)
            setScroll(r.top() - 4);
        else if (r.bottom() + 4 > m_scroll + height())
            setScroll(r.bottom() + 4 - height() + 1);
    }

    void setFocusId(const QString& id)
    {
        m_panel->m_focus        = id;
        m_panel->m_keyboard     = true;
        m_panel->m_hoverWasLast = false;
        m_panel->m_hasNote      = false;
        const int index         = m_panel->m_order.indexOf(id);
        if (index >= 0)
            ensureVisible(index);
        const gifs::Item* item = m_panel->item(id);
        setAccessibleName(item ? m_panel->labelOf(*item) : i18n::t("GIFs"));
        if (QAccessible::isActive()) {
            QAccessibleEvent event(this, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        }
        m_panel->setTarget(id);
        m_panel->update();
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter                   p(this);
        const pickerstyle::Colors& c = *m_panel->m_colors;
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal dpr = devicePixelRatioF();
        if (m_panel->m_order.isEmpty()) {
            paintEmpty(p);
            return;
        }
        const QSize device(qRound(GifPanel::kTileW * dpr), qRound(GifPanel::kTileH * dpr));
        for (int i = 0; i < m_panel->m_order.size(); ++i) {
            const QRect r = tile(i);
            if (r.bottom() < 0 || r.top() > height())
                continue;
            const QString     id   = m_panel->m_order.at(i);
            const gifs::Item* item = m_panel->item(id);
            if (!item)
                continue;
            paintTile(p, c, *item, r, device);
        }
        // Ahead: the next screen's thumbnails (decoded on the worker while nothing waits for them).
        if (GifLibrary* lib = m_panel->m_lib) {
            for (int i = 0; i < m_panel->m_order.size(); ++i) {
                const QRect r = tile(i);
                if (r.top() > height() && r.top() <= 2 * height()) {
                    const gifs::Item* item = m_panel->item(m_panel->m_order.at(i));
                    if (item && !item->thumb.isEmpty())
                        lib->thumbnail(item->id, device, m_panel->covered(*item));
                }
            }
        }
        if (contentHeight() > height()) {
            const QRectF bar = thumbRect();
            p.setPen(Qt::NoPen);
            p.setBrush(m_dragging || m_overBar ? c.muted : c.thumb);
            p.drawRoundedRect(bar, bar.width() / 2, bar.width() / 2);
        }
    }

    void paintTile(QPainter& p, const pickerstyle::Colors& c, const gifs::Item& item, const QRect& r, const QSize& device)
    {
        const QString block    = m_panel->blockOf(item);
        const bool    disabled = !block.isEmpty();
        const bool    cover    = m_panel->covered(item);
        p.save();
        if (disabled)
            p.setOpacity(0.4);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(r), 6, 6);
        p.setClipPath(clip);
        bool drawn = false;
        if (m_panel->m_anim == item.id && !cover) {
            if (!m_panel->m_frames.isEmpty()) {
                const QImage& frame = m_panel->m_frames.at(qBound(0, m_panel->m_frame, m_panel->m_frames.size() - 1));
                p.drawImage(QRectF(r), frame, coverSource(frame.size(), QSizeF(frame.size())));
                ++m_panel->m_framePaints;
                drawn = true;
            } else if (m_panel->m_player && m_panel->m_player->isPlaying()) {
                const QImage frame = m_panel->m_player->currentFrame();
                if (!frame.isNull()) {
                    p.drawImage(QRectF(r), frame, coverSource(frame.size(), QSizeF(r.size()) * devicePixelRatioF()));
                    ++m_panel->m_framePaints;
                    drawn = true;
                }
            }
        }
        if (!drawn) {
            if (item.thumb.isEmpty()) {
                // The placeholder: "GIF" and the link's host.
                p.fillRect(r, c.field);
                p.setPen(c.muted);
                p.setFont(scaled(font(), 15, QFont::Bold));
                const QRect label(r.left(), r.center().y() - 6 - 12, r.width(), 22);
                p.drawText(label, Qt::AlignCenter, QString::fromLatin1("GIF"));
                const QString host = item.source == gifs::Source::Link ? m_panel->hostOf(item) : item.name;
                if (!host.isEmpty()) {
                    const QFont small = scaled(font(), 11, QFont::Normal);
                    p.setFont(small);
                    p.drawText(QRect(r.left() + 6, label.bottom() + 2, r.width() - 12, 16), Qt::AlignHCenter | Qt::AlignTop,
                               QFontMetrics(small).elidedText(host, Qt::ElideMiddle, r.width() - 12));
                }
            } else {
                const QImage picture = m_panel->m_lib ? m_panel->m_lib->thumbnail(item.id, device, cover) : QImage();
                if (picture.isNull())
                    p.fillRect(r, c.dot); // until the worker has decoded it
                else
                    p.drawImage(QRectF(r), picture);
            }
        }
        if (cover) {
            p.fillRect(r, QColor(0, 0, 0, 90));
            const QFont   chipFont = scaled(font(), 11, QFont::DemiBold);
            const QString chip     = i18n::t("Spoiler");
            const QFontMetrics fm(chipFont);
            const QRectF box(r.left() + 5, r.top() + 5, fm.horizontalAdvance(chip) + 10, fm.height() + 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(Qt::white);
            p.setFont(chipFont);
            p.drawText(box, Qt::AlignCenter, chip);
        }
        if (item.source == gifs::Source::File) {
            const QRectF badge(r.left() + 5, r.bottom() - 22, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawEllipse(badge);
            drawUploadGlyph(p, QRectF(badge.center().x() - 5.5, badge.center().y() - 5.5, 11, 11), Qt::white);
        }
        if (m_panel->m_press == item.id && !disabled)
            p.fillRect(r, QColor(0, 0, 0, 50));
        p.setClipping(false);
        const bool focused = m_panel->m_focus == item.id && m_panel->m_keyboard && hasFocus();
        if (m_panel->m_hover == item.id || focused) {
            p.setOpacity(1.0);
            p.setPen(QPen(focused ? c.ring : c.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 5, 5);
        }
        p.restore();
    }

    void paintEmpty(QPainter& p)
    {
        const pickerstyle::Colors& c = *m_panel->m_colors;
        QString                    title, body;
        GifLibrary*                lib = m_panel->m_lib;
        if (lib && lib->state() == GifLibrary::State::ReadOnly) {
            title = i18n::t("GIFs unavailable");
            // "TS Media" with a no-break space: the wrapped body never splits the product name.
            body  = i18n::t("Your GIF list was saved by a newer version of TS\xC2\xA0Media. Update TS\xC2\xA0Media to use it. It was left unchanged.");
        } else if (lib && lib->state() == GifLibrary::State::Unavailable) {
            title = i18n::t("GIFs unavailable");
            body  = i18n::t("Your GIF list couldn't be opened right now. TS\xC2\xA0Media tries again the next time TeamSpeak starts. It was left unchanged.");
        } else if (!Settings::instance().rememberGifs) {
            title = i18n::t("No GIFs yet");
            body  = i18n::t("Remembering the GIFs you send is off. Right-click a GIF in the chat and choose Save to GIFs to keep it here.");
        } else {
            title = i18n::t("No GIFs yet");
            body  = i18n::t("GIF links you send from Giphy, Tenor, Klipy, Imgur and other GIF sites show up here, newest first, and so do GIF files "
                           "you upload. To keep any other GIF, right-click it in the chat and choose Save to GIFs.");
        }
        const QFont titleFont = scaled(font(), 14, QFont::DemiBold);
        const QFont bodyFont  = scaled(font(), 12.5, QFont::Normal);
        const int   textW     = qMin(300, width() - 32);
        const QRect bodyBox   = QFontMetrics(bodyFont).boundingRect(QRect(0, 0, textW, 1000), Qt::AlignHCenter | Qt::TextWordWrap, body);
        const int   titleH    = QFontMetrics(titleFont).height();
        const int   total     = 30 + 12 + titleH + 6 + bodyBox.height();
        int         y         = (height() - total) / 2;
        const QRectF icon((width() - 44) / 2.0, y, 44, 30);
        p.setPen(QPen(c.muted, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(icon.adjusted(0.8, 0.8, -0.8, -0.8), 6, 6);
        p.setFont(scaled(font(), 12, QFont::Bold));
        p.drawText(icon, Qt::AlignCenter, QString::fromLatin1("GIF"));
        y += 30 + 12;
        p.setPen(c.text);
        p.setFont(titleFont);
        p.drawText(QRect(0, y, width(), titleH), Qt::AlignHCenter | Qt::AlignVCenter, title);
        y += titleH + 6;
        p.setPen(c.muted);
        p.setFont(bodyFont);
        p.drawText(QRect((width() - textW) / 2, y, textW, bodyBox.height()), Qt::AlignHCenter | Qt::TextWordWrap, body);
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip) {
            // The tile's own text (never an emoji category's).
            auto*             he   = static_cast<QHelpEvent*>(event);
            const int         at   = indexAt(he->pos());
            const gifs::Item* item = m_panel->item(m_panel->m_order.value(at));
            if (item)
                QToolTip::showText(he->globalPos(), GifPanel::toolTipHtml(m_panel->toolTipOf(*item), true), this, tile(at));
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging) {
            const qreal track = height() - thumbRect().height();
            if (track > 0)
                setScroll(m_dragScroll + qRound((event->pos().y() - m_dragY) * (contentHeight() - height()) / track));
            return;
        }
        const bool over = contentHeight() > height() && event->pos().x() >= width() - kBar - 2;
        if (over != m_overBar) {
            m_overBar = over;
            update();
        }
        setHover(over ? QString() : idAt(event->pos()));
    }

    void leaveEvent(QEvent*) override
    {
        m_overBar = false;
        setHover(QString());
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (contentHeight() > height() && event->pos().x() >= width() - kBar - 2) {
            const QRectF thumb = thumbRect();
            if (!thumb.contains(event->pos()))
                setScroll(m_scroll + (event->pos().y() < thumb.top() ? -height() : height()));
            m_dragging   = true;
            m_dragY      = event->pos().y();
            m_dragScroll = m_scroll;
            return;
        }
        // Right after the page showed (a mouse opening that remembered GIFs, a page switch): no press, so no send. A
        // habitual click where the emoji page's recent emoji sit must not post a GIF to the chat (review fix).
        m_panel->m_press = m_panel->pressGuarded() ? QString() : idAt(event->pos()); // activation is by this id until the release
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_dragging) {
            m_dragging = false;
            update();
            return;
        }
        const QString pressed = m_panel->m_press;
        m_panel->m_press.clear();
        update();
        if (event->button() != Qt::LeftButton || pressed.isEmpty() || idAt(event->pos()) != pressed || !m_panel->item(pressed))
            return;
        m_panel->activate(pressed, event->modifiers() & Qt::ShiftModifier, event->globalPos());
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        if (event->reason() == QContextMenuEvent::Mouse) {
            m_panel->openMenu(event->globalPos(), idAt(event->pos()));
        } else {
            const int at = m_panel->m_order.indexOf(m_panel->m_focus);
            m_panel->openMenu(at >= 0 ? mapToGlobal(tile(at).center()) : mapToGlobal(rect().center()), at >= 0 ? m_panel->m_focus : QString());
        }
        event->accept();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        const QPoint pixels = event->pixelDelta();
        const int    delta  = !pixels.isNull() ? pixels.y() : event->angleDelta().y() * GifPanel::kCellH / 120;
        setScroll(m_scroll - delta);
        setHover(idAt(event->position().toPoint()));
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const QStringList& order = m_panel->m_order;
        const int          count = order.size();
        const int          at    = order.indexOf(m_panel->m_focus);
        const auto         move  = [&](int to) {
            if (count > 0)
                setFocusId(order.at(qBound(0, to, count - 1)));
        };
        switch (event->key()) {
        case Qt::Key_Left:
            move(at < 0 ? 0 : at - 1);
            return;
        case Qt::Key_Right:
            move(at < 0 ? 0 : at + 1);
            return;
        case Qt::Key_Down:
            move(at < 0 ? 0 : (at + GifPanel::kColumns < count ? at + GifPanel::kColumns : at));
            return;
        case Qt::Key_Up:
            if (at < 0)
                move(0);
            else if (at < GifPanel::kColumns)
                emit m_panel->upFromGrid();
            else
                move(at - GifPanel::kColumns);
            return;
        case Qt::Key_Home:
            move(0);
            return;
        case Qt::Key_End:
            move(count - 1);
            return;
        case Qt::Key_PageDown:
            move(at < 0 ? 0 : at + 4 * GifPanel::kColumns);
            return;
        case Qt::Key_PageUp:
            move(at < 0 ? 0 : at - 4 * GifPanel::kColumns);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Only after keyboard navigation in this opening (a stray Enter right after opening does nothing).
            if (m_panel->m_keyboard && at >= 0)
                m_panel->activate(m_panel->m_focus, event->modifiers() & Qt::ShiftModifier, mapToGlobal(tile(at).center()));
            return;
        case Qt::Key_Space:
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            return; // never sends, never removes
        case Qt::Key_Escape:
            if (m_panel->confirmShown())
                m_panel->hideConfirm();
            else
                emit m_panel->closeRequested();
            return;
        case Qt::Key_Menu:
            contextMenuKey();
            return;
        case Qt::Key_F10:
            if (event->modifiers() & Qt::ShiftModifier) {
                contextMenuKey();
                return;
            }
            break;
        default:
            break;
        }
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint() && text.at(0) != QLatin1Char(' ') && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            emit m_panel->typed(text); // to the emoji search
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent* event) override
    {
        // Tab into the grid: the first item with the ring. Any other focus (the opening): nothing focused.
        if ((event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason) && !m_panel->m_order.isEmpty())
            setFocusId(m_panel->m_focus.isEmpty() ? m_panel->m_order.first() : m_panel->m_focus);
        update();
    }
    void focusOutEvent(QFocusEvent*) override { update(); }
    void resizeEvent(QResizeEvent*) override { setScroll(m_scroll); }

  private:
    void contextMenuKey()
    {
        const int at = m_panel->m_order.indexOf(m_panel->m_focus);
        m_panel->openMenu(at >= 0 ? mapToGlobal(tile(at).center()) : mapToGlobal(rect().center()), at >= 0 ? m_panel->m_focus : QString());
    }

    void setHover(const QString& id)
    {
        if (id == m_panel->m_hover)
            return;
        m_panel->m_hover        = id;
        m_panel->m_hoverWasLast = true;
        m_panel->m_hasNote      = false;
        const gifs::Item* item = m_panel->item(id);
        setCursor(!item || !m_panel->blockOf(*item).isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
        m_panel->setTarget(id.isEmpty() && m_panel->m_keyboard ? m_panel->m_focus : id);
        m_panel->update(); // the footer
        update();
    }

    QRectF thumbRect() const
    {
        const qreal visible = static_cast<qreal>(height()) / qMax(1, contentHeight());
        const qreal h       = qMax<qreal>(28, height() * visible);
        const int   range   = qMax(1, contentHeight() - height());
        const qreal y       = (height() - h) * m_scroll / range;
        return QRectF(width() - kBar + 1, y + 2, kBar - 3, h - 4);
    }

    GifPanel* m_panel;
    int       m_scroll     = 0;
    bool      m_dragging   = false;
    bool      m_overBar    = false;
    int       m_dragY      = 0;
    int       m_dragScroll = 0;
};

// ============================================================================================
// "Remove all N GIFs?" inline (no modal dialog inside the popup)
// ============================================================================================

class GifBarButton : public QAbstractButton
{
  public:
    GifBarButton(const pickerstyle::Colors* colors, const QString& text, bool filled, QWidget* parent)
        : QAbstractButton(parent)
        , m_colors(colors)
        , m_filled(filled)
    {
        setText(text);
        setAccessibleName(text);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        const QFontMetrics fm(scaled(font(), 13, QFont::DemiBold));
        setFixedSize(fm.horizontalAdvance(text) + 24, 28);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (m_filled) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? m_colors->dangerFill.darker(115) : m_colors->dangerFill);
            p.drawRoundedRect(box, 6, 6);
        } else if (underMouse() || isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? m_colors->pressed : m_colors->hover);
            p.drawRoundedRect(box, 6, 6);
        }
        if (hasFocus()) {
            p.setPen(QPen(m_colors->ring, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 6, 6);
        }
        p.setPen(m_filled ? QColor(Qt::white) : m_colors->text);
        p.setFont(scaled(font(), 13, QFont::DemiBold));
        p.drawText(rect(), Qt::AlignCenter, text());
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            click();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

  private:
    const pickerstyle::Colors* m_colors;
    bool                       m_filled;
};

class GifConfirmBar : public QWidget
{
  public:
    explicit GifConfirmBar(GifPanel* panel)
        : QWidget(panel)
        , m_panel(panel)
    {
        setObjectName(QString::fromLatin1("tsmediaGifConfirm"));
        m_cancel = new GifBarButton(panel->m_colors, i18n::t("Cancel"), false, this);
        m_remove = new GifBarButton(panel->m_colors, i18n::t("Remove all"), true, this);
        m_cancel->setObjectName(QString::fromLatin1("tsmediaGifCancel"));
        m_remove->setObjectName(QString::fromLatin1("tsmediaGifRemoveAll"));
        QObject::connect(m_cancel, &QAbstractButton::clicked, panel, [panel] { panel->hideConfirm(); });
        QObject::connect(m_remove, &QAbstractButton::clicked, panel, [panel] { panel->confirmRemoveAll(); });
        hide();
    }

    QAbstractButton* cancel() const { return m_cancel; }
    QAbstractButton* removeAll() const { return m_remove; }

    QString text() const { return GifPanel::removeAllQuestion(m_panel->m_lib ? m_panel->m_lib->count() : 0); }

  protected:
    void resizeEvent(QResizeEvent*) override
    {
        m_remove->move(width() - 8 - m_remove->width(), (height() - m_remove->height()) / 2);
        m_cancel->move(m_remove->x() - 6 - m_cancel->width(), (height() - m_cancel->height()) / 2);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_panel->m_colors->field);
        p.drawRoundedRect(QRectF(rect()), 8, 8);
        p.setPen(m_panel->m_colors->text);
        const QFont f = scaled(font(), 13, QFont::Normal);
        p.setFont(f);
        const int textW = m_cancel->x() - 12 - 8;
        p.drawText(QRect(12, 0, textW, height()), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(text(), Qt::ElideRight, textW));
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_panel->hideConfirm();
            return;
        }
        QWidget::keyPressEvent(event);
    }

  private:
    GifPanel*     m_panel;
    GifBarButton* m_cancel;
    GifBarButton* m_remove;
};

// ============================================================================================
// The panel
// ============================================================================================

GifPanel::GifPanel(const pickerstyle::Colors* colors, const gifs::Host& host, const gifs::SendState& opened, QWidget* parent)
    : QWidget(parent)
    , m_colors(colors)
    , m_host(host)
    , m_lib(host.library)
    , m_opened(opened)
    , m_state(opened)
{
    setObjectName(QString::fromLatin1("tsmediaGifPanel"));
    // The snapshot: nothing reorders while the picker is open (design_gifs 5.3).
    if (m_lib) {
        for (const gifs::Item& item : m_lib->items())
            m_order.append(item.id);
    }
    m_grid = new GifGrid(this);
    m_bar  = new GifConfirmBar(this);
    m_dwell = new QTimer(this);
    m_dwell->setSingleShot(true);
    connect(m_dwell, &QTimer::timeout, this, [this] { startAnimation(); });
    m_frameTimer = new QTimer(this);
    m_frameTimer->setSingleShot(true);
    connect(m_frameTimer, &QTimer::timeout, this, [this] { nextFrame(); });
    if (m_lib) {
        connect(m_lib.data(), &GifLibrary::changed, this, [this] { libraryChanged(); });
        connect(m_lib.data(), &GifLibrary::itemUpdated, this, [this](const QString& id) {
            if (id == m_anim)
                stopAnimation();
            m_grid->update();
            update();
        });
        connect(m_lib.data(), &GifLibrary::thumbReady, this, [this](const QString&) {
            m_grid->update();
            update();
        });
        connect(m_lib.data(), &GifLibrary::animationReady, this, [this](const QString& id) { onAnimationReady(id); });
    }
    layoutChildren();
}

GifPanel::~GifPanel()
{
    if (m_lib)
        m_lib->cancelAnimation();
    delete m_player; // its engine shuts down synchronously, at most once per opening
    m_player = nullptr;
}

void GifPanel::setAnimationsEnabledForTests(int enabled)
{
    g_animationsForTests = enabled;
}

QWidget* GifPanel::grid() const
{
    return m_grid;
}

QList<QWidget*> GifPanel::keyWidgets() const
{
    return {m_grid, m_bar->cancel(), m_bar->removeAll(), m_bar};
}

bool GifPanel::confirmShown() const
{
    return !m_bar->isHidden();
}

void GifPanel::layoutChildren()
{
    const bool bar = !m_bar->isHidden();
    m_bar->setGeometry(8, kTop, kGridW - kBar, kBarH);
    const int top = kTop + (bar ? kBarH + 8 : 0);
    m_grid->setGeometry(8, top, kGridW, kGridH - (top - kTop));
}

void GifPanel::resizeEvent(QResizeEvent*)
{
    layoutChildren();
}

void GifPanel::showConfirm()
{
    if (!m_lib || m_lib->state() != GifLibrary::State::Ok)
        return;
    m_bar->show();
    layoutChildren();
    m_bar->cancel()->setFocus(Qt::OtherFocusReason); // never the destructive one first
    update();
}

void GifPanel::hideConfirm()
{
    if (m_bar->isHidden())
        return;
    m_bar->hide();
    layoutChildren();
    m_grid->setFocus(Qt::OtherFocusReason);
    update();
}

void GifPanel::confirmRemoveAll()
{
    stopAnimation();
    if (m_lib)
        m_lib->clear();
    hideConfirm();
}

void GifPanel::focusGrid(Qt::FocusReason reason)
{
    m_grid->setFocus(reason);
}

void GifPanel::setShown(bool shown)
{
    if (!shown) {
        stopAnimation();
        m_target.clear();
        return;
    }
    m_shownClock.restart(); // a click right after a page switch doesn't send (kOpenGuardMs)
}

void GifPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_shownClock.restart(); // the picker opened on this page (a mouse opening that remembered it)
}

bool GifPanel::pressGuarded() const
{
    return m_shownClock.isValid() && m_shownClock.elapsed() < kOpenGuardMs;
}

QString GifPanel::removeAllQuestion(int count)
{
    if (count == 0)
        return i18n::t("Remove the leftover GIF copies?");
    if (count == 1)
        return i18n::t("Remove the GIF?");
    return i18n::t("Remove all %1 GIFs?").arg(count);
}

QString GifPanel::toolTipHtml(const QString& text, bool oneLinePerLine)
{
    if (text.isEmpty())
        return text;
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines)
        line = line.toHtmlEscaped();
    // QToolTip shows text that looks like HTML as rich text (a name like "<img src=...>.gif" would be drawn, and its
    // picture loaded): always rich text of our own, escaped (ChatIntegration::toolTipText, uploadtoast's plainToolTip).
    return (oneLinePerLine ? QString::fromLatin1("<p style='white-space:pre'>") : QString::fromLatin1("<p>")) + lines.join(QString::fromLatin1("<br>"))
           + QString::fromLatin1("</p>");
}

QString GifPanel::toolTipFor(int index) const
{
    const gifs::Item* it = item(m_order.value(index));
    return it ? toolTipOf(*it) : QString();
}

void GifPanel::refreshState()
{
    if (m_host.state)
        m_state = m_host.state();
    if (m_state.chat != m_opened.chat)
        m_changed = true;
    m_grid->update();
    update();
}

const gifs::Item* GifPanel::item(const QString& id) const
{
    return m_lib && !id.isEmpty() ? m_lib->find(id) : nullptr;
}

QString GifPanel::blockOf(const gifs::Item& item) const
{
    if (m_changed)
        return gifs::changedText();
    return m_lib ? m_lib->blockOf(item, m_state) : gifs::changedText();
}

QString GifPanel::hostOf(const gifs::Item& item) const
{
    return m_lib ? m_lib->hostOf(item) : QString();
}

QString GifPanel::labelOf(const gifs::Item& item) const
{
    QString label = item.source == gifs::Source::Link ? i18n::t("GIF link from %1").arg(hostOf(item))
                                                      : i18n::t("GIF file %1, %2").arg(item.name, formatSize(static_cast<quint64>(item.size)));
    if (covered(item))
        label += i18n::t(", spoiler");
    const QString block = blockOf(item);
    if (!block.isEmpty())
        label += i18n::t(", unavailable: %1").arg(block);
    return label;
}

QString GifPanel::toolTipOf(const gifs::Item& item) const
{
    const QString block = blockOf(item);
    if (!block.isEmpty())
        return block;
    if (item.source == gifs::Source::Link)
        return hostOf(item) + QLatin1Char('\n') + i18n::t("Click to send · Shift+click keeps this open");
    return item.name + dot() + formatSize(static_cast<quint64>(item.size)) + QLatin1Char('\n') + i18n::t("Uploads the file again");
}

QRect GifPanel::tileRect(int index) const
{
    if (index < 0 || index >= m_order.size())
        return {};
    return m_grid->tile(index).translated(m_grid->pos());
}

bool GifPanel::animating() const
{
    return !m_frames.isEmpty() || (m_player && m_player->isPlaying());
}

// ---- the footer -------------------------------------------------------------------------------------------------------

QString GifPanel::footerSubject() const
{
    if (m_hoverWasLast && !m_hover.isEmpty())
        return m_hover;
    if (m_keyboard && !m_focus.isEmpty() && m_grid->hasFocus())
        return m_focus;
    return m_hover;
}

GifPanel::Note GifPanel::footerNote() const
{
    if (m_hasNote)
        return m_note;
    Note              note;
    const gifs::Item* it = item(footerSubject());
    if (!it) {
        if (m_changed) {
            note.title  = i18n::t("Can't send here");
            note.text   = gifs::changedText();
            note.danger = true;
        } else if (!m_state.canText) {
            note.title  = i18n::t("Can't send here");
            note.text   = m_state.blocked;
            note.danger = true;
        } else {
            note.title = i18n::t("Send to %1").arg(m_state.destination);
            note.text  = m_state.serverChat ? i18n::t("Everyone on this server gets it.") : i18n::t("Click to send · Shift+click keeps this open");
        }
        return note;
    }
    note.title          = it->source == gifs::Source::Link ? hostOf(*it) : it->name;
    const QString block = blockOf(*it);
    if (!block.isEmpty()) {
        note.text   = block;
        note.danger = true;
        return note;
    }
    if (it->source == gifs::Source::Link) {
        note.text = (it->how == gifs::How::Sent ? i18n::t("Link · sent %1") : i18n::t("Link · saved %1")).arg(gifs::dateText(it->addedSec));
    } else if (m_state.serverChat) {
        note.text = gifs::serverFileText();
    } else {
        note.text = i18n::t("Uploads the file again · %1").arg(formatSize(static_cast<quint64>(it->size)));
    }
    if (covered(*it))
        note.text = i18n::t("Spoiler · %1").arg(note.text);
    return note;
}

QString GifPanel::footerTitle() const
{
    return footerNote().title;
}

QString GifPanel::footerText() const
{
    return footerNote().text;
}

bool GifPanel::footerDanger() const
{
    return footerNote().danger;
}

void GifPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r(0, height() - kFooterH, width(), kFooterH);
    p.setPen(QPen(m_colors->divider, 1.0));
    p.drawLine(QPointF(0, r.top() + 0.5), QPointF(width(), r.top() + 0.5));
    const Note        note = footerNote();
    const gifs::Item* it   = m_hasNote ? nullptr : item(footerSubject());
    int               left = 14;
    if (it) {
        const QRectF box(14, r.top() + 8, 48, 36);
        const qreal  dpr     = devicePixelRatioF();
        const QImage picture = m_lib && !it->thumb.isEmpty() ? m_lib->thumbnail(it->id, QSize(qRound(48 * dpr), qRound(36 * dpr)), covered(*it)) : QImage();
        QPainterPath clip;
        clip.addRoundedRect(box, 4, 4);
        p.save();
        p.setClipPath(clip);
        if (picture.isNull())
            p.fillRect(box, m_colors->field);
        else
            p.drawImage(box, picture);
        p.restore();
        left = 74;
    }
    const int   textW    = width() - left - 14;
    const QFont nameFont = scaled(font(), 13.5, QFont::DemiBold);
    const QFont lineFont = scaled(font(), 12, QFont::Normal);
    const QFontMetrics nm(nameFont);
    const QFontMetrics lm(lineFont);
    const QStringList  lines = footerLines(note, lineFont, textW);
    const int          top   = r.top() + (kFooterH - nm.height() - 1 - lm.height() * lines.size()) / 2;
    p.setFont(nameFont);
    p.setPen(m_colors->text);
    p.drawText(QRect(left, top, textW, nm.height()), Qt::AlignLeft | Qt::AlignVCenter, nm.elidedText(note.title, Qt::ElideRight, textW));
    p.setFont(lineFont);
    p.setPen(note.danger ? m_colors->danger : m_colors->muted);
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(QRect(left, top + nm.height() + 1 + i * lm.height(), textW, lm.height()), Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
}

QStringList GifPanel::footerLines(const Note& note, const QFont& font, int width)
{
    // Line 2: one line; a text that doesn't fit gets a second one (52 px hold the name and two 12 px lines), so a reason
    // such as "Sending files to password-protected channels isn't supported..." (review fix) and the server chat's file
    // hint "Uploads to your channel's files; others may lack access." beside the mini thumbnail (live fix: it ended
    // "others may lack acc...") are read whole; the last line elided only beyond two.
    const QFontMetrics fm(font);
    if (fm.horizontalAdvance(note.text) <= width || width <= 0)
        return {fm.elidedText(note.text, Qt::ElideRight, width)};
    QTextLayout layout(note.text, font);
    layout.beginLayout();
    QStringList lines;
    for (QTextLine line = layout.createLine(); line.isValid() && lines.size() < 2; line = layout.createLine()) {
        line.setLineWidth(width);
        const QString part = note.text.mid(line.textStart(), line.textLength()).trimmed();
        if (lines.size() == 1)
            lines << fm.elidedText(note.text.mid(line.textStart()).trimmed(), Qt::ElideRight, width); // the rest, cut if need be
        else
            lines << part;
    }
    layout.endLayout();
    return lines.isEmpty() ? QStringList{fm.elidedText(note.text, Qt::ElideRight, width)} : lines;
}

QStringList GifPanel::footerTextLines() const
{
    int left = 14;
    if (!m_hasNote && item(footerSubject()))
        left = 74;
    return footerLines(footerNote(), scaled(font(), 12, QFont::Normal), width() - left - 14);
}

// ---- sending -------------------------------------------------------------------------------------------------------

void GifPanel::activate(const QString& id, bool keepOpen, const QPoint& globalTip)
{
    const gifs::Item* it = item(id);
    if (!it || !m_host.send)
        return;
    if (m_host.state)
        m_state = m_host.state(); // checked again at each click (disconnected, a password set, a refusal)
    const QString block = blockOf(*it);
    const auto    say   = [this, &globalTip](const QString& title, const QString& text) {
        m_note    = Note{title, text, true};
        m_hasNote = true;
        QToolTip::showText(globalTip, toolTipHtml(text, false), m_grid, QRect(), ui::notificationDurationMs()); // a server's refusal text: escaped
        m_grid->update();
        update();
    };
    if (!block.isEmpty()) {
        say(m_changed ? i18n::t("Can't send here") : (it->source == gifs::Source::Link ? hostOf(*it) : it->name), block);
        return;
    }
    QString    error;
    const bool ok = m_host.send(id, m_opened.chat, &error);
    if (!ok) {
        if (error == gifs::changedText()) {
            m_changed = true; // every tile is disabled for the rest of this opening
            say(i18n::t("Can't send here"), error);
        } else {
            say(i18n::t("Couldn't send the GIF"), error.isEmpty() ? i18n::t("Try again.") : error);
        }
        return;
    }
    m_hasNote = false;
    emit sent(keepOpen);
}

bool GifPanel::sendIndex(int index, bool keepOpen)
{
    const QString id = m_order.value(index);
    if (id.isEmpty() || !item(id))
        return false;
    bool sentNow = false;
    const QMetaObject::Connection c = connect(this, &GifPanel::sent, this, [&sentNow](bool) { sentNow = true; });
    activate(id, keepOpen, mapToGlobal(tileRect(index).center()));
    disconnect(c);
    return sentNow;
}

void GifPanel::hoverIndex(int index)
{
    const QString id = m_order.value(index);
    m_hover          = id;
    m_hoverWasLast   = true;
    m_hasNote        = false;
    setTarget(id);
    m_grid->update();
    update();
}

void GifPanel::scrollTo(int contentY)
{
    m_grid->setScroll(contentY);
}

// ---- the menu --------------------------------------------------------------------------------------------------------

void GifPanel::openMenu(const QPoint& globalPos, const QString& id)
{
    auto* menu = new QMenu(window());
    menu->setObjectName(QString::fromLatin1("tsmediaGifMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    QPointer<GifPanel> self(this);
    const gifs::Item*  it       = item(id);
    const bool         editable = m_lib && m_lib->state() == GifLibrary::State::Ok;
    if (it) {
        const QString block = blockOf(*it);
        QAction*      send  = menu->addAction(i18n::t("&Send"), this, [self, id] {
            if (self) {
                const int at = self->m_order.indexOf(id);
                self->activate(id, false, self->mapToGlobal(self->tileRect(at).center()));
            }
        });
        send->setEnabled(block.isEmpty());
        send->setToolTip(block);
        menu->setDefaultAction(send);
        if (it->source == gifs::Source::Link) {
            const QString url = it->url;
            menu->addAction(i18n::t("&Copy link"), this, [self, url] {
                QGuiApplication::clipboard()->setText(url);
                if (self)
                    QToolTip::showText(QCursor::pos(), i18n::t("Link copied"), self->m_grid);
            });
        }
        if (covered(*it))
            menu->addAction(i18n::t("Re&veal"), this, [self, id] {
                if (self)
                    self->reveal(id);
            });
        menu->addSeparator();
        menu->addAction(i18n::t("&Remove"), this, [self, id] {
            if (self && self->m_lib)
                self->m_lib->remove(id);
        })->setEnabled(editable);
    }
    menu->addAction(i18n::t("Remove &all GIFs…"), this, [self] {
            if (self)
                self->showConfirm();
        })->setEnabled(editable && m_lib->hasAnythingToRemove()); // leftover copies of an unreadable list too
    menu->addSeparator();
    QAction* remember = menu->addAction(i18n::t("Re&member GIFs I send"), this, [self](bool on) {
        Settings::instance().rememberGifs = on;
        Settings::instance().save();
        if (self) {
            self->m_grid->update();
            self->update();
        }
    });
    remember->setCheckable(true);
    remember->setChecked(Settings::instance().rememberGifs);
    menu->setToolTipsVisible(true);
    menu->popup(globalPos); // never exec(): no nested event loop inside the popup
}

void GifPanel::removeIndex(int index)
{
    const QString id = m_order.value(index);
    if (!id.isEmpty() && m_lib)
        m_lib->remove(id);
}

void GifPanel::reveal(const QString& id)
{
    m_revealed.insert(id);
    m_noAnimation.remove(id);
    if (m_target == id)
        startAnimation();
    m_grid->update();
    update();
}

void GifPanel::libraryChanged()
{
    // Removed items leave at once; new and reordered ones show at the next opening.
    QStringList kept;
    for (const QString& id : qAsConst(m_order)) {
        if (item(id))
            kept.append(id);
    }
    if (kept.size() != m_order.size()) { // kept is m_order without the gone ones
        m_order = kept;
        if (!item(m_anim))
            stopAnimation();
        if (!item(m_focus))
            m_focus.clear(); // nothing is focused until the next navigation key
        if (!item(m_hover))
            m_hover.clear();
        if (!item(m_press))
            m_press.clear();
        if (!item(m_target))
            m_target.clear();
        m_grid->setScroll(m_grid->scroll());
    }
    m_grid->update();
    update();
}

// ---- the animation (5.7) -----------------------------------------------------------------------------------------------

void GifPanel::setTarget(const QString& id)
{
    if (id == m_target)
        return;
    stopAnimation();
    m_target = id;
    if (id.isEmpty())
        return;
    m_dwell->start(animationsOn() ? kDwellMs : kSlowDwellMs); // a pointer passing over tiles never animates them
}

void GifPanel::startAnimation()
{
    const gifs::Item* it = item(m_target);
    if (!it || !m_lib || covered(*it) || m_noAnimation.contains(it->id) || !isVisible())
        return;
    gifs::Kind    kind   = gifs::Kind::Gif;
    const QString source = m_lib->animationSource(it->id, &kind);
    if (source.isEmpty())
        return;
    m_anim = it->id;
    if (kind == gifs::Kind::Loop) {
        // One engine per opening, only for loops of a sane size and length (unknown: the still).
        if (!mf::available() || it->width <= 0 || it->height <= 0 || it->width > kMaxLoopW || it->height > kMaxLoopH || it->durationMs <= 0
            || it->durationMs > kMaxLoopMs) {
            m_anim.clear();
            return;
        }
        ensurePlayer();
        const qreal  dpr = devicePixelRatioF();
        const qreal  s   = qMax(kTileW * dpr / it->width, kTileH * dpr / it->height);
        m_player->setFrameSize(QSize(qMax(1, qRound(it->width * s)), qMax(1, qRound(it->height * s))));
        m_player->open(source);
        m_player->setMuted(true);
        m_player->setLoop(true);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    m_lib->requestAnimation(it->id, QSize(qRound(kTileW * dpr), qRound(kTileH * dpr)));
}

void GifPanel::ensurePlayer()
{
    if (m_player)
        return;
    m_player = new mf::VideoPlayer(this);
    ++m_playersCreated;
    connect(m_player, &mf::VideoPlayer::loaded, this, [this] {
        if (!m_anim.isEmpty() && m_player) {
            m_player->setMuted(true);
            m_player->play();
        }
    });
    connect(m_player, &mf::VideoPlayer::frameReady, this, [this] {
        const int at = m_order.indexOf(m_anim);
        if (at >= 0)
            m_grid->update(m_grid->tile(at));
    });
    connect(m_player, &mf::VideoPlayer::failed, this, [this](const QString&) {
        m_noAnimation.insert(m_anim); // the still for this opening
        stopAnimation();
    });
}

void GifPanel::onAnimationReady(const QString& id)
{
    if (id != m_anim || !m_lib)
        return;
    QVector<QImage> frames;
    QVector<int>    delays;
    if (!m_lib->takeAnimation(id, &frames, &delays) || frames.isEmpty()) {
        m_noAnimation.insert(id);
        m_anim.clear();
        return;
    }
    m_frames = frames;
    m_delays = delays;
    m_frame  = 0;
    const int at = m_order.indexOf(id);
    if (at >= 0)
        m_grid->update(m_grid->tile(at));
    if (m_frames.size() > 1)
        m_frameTimer->start(qMax(kMinFrameMs, m_delays.value(0, 100)));
}

void GifPanel::nextFrame()
{
    if (m_frames.size() < 2)
        return;
    m_frame      = (m_frame + 1) % m_frames.size();
    const int at = m_order.indexOf(m_anim);
    if (at >= 0)
        m_grid->update(m_grid->tile(at));
    m_frameTimer->start(qMax(kMinFrameMs, m_delays.value(m_frame, 100)));
}

void GifPanel::stopAnimation()
{
    m_dwell->stop();
    m_frameTimer->stop();
    const int at = m_order.indexOf(m_anim);
    m_frames.clear();
    m_delays.clear();
    m_frame = 0;
    if (!m_anim.isEmpty() && m_lib)
        m_lib->cancelAnimation();
    if (m_player)
        m_player->pause(); // the engine stays for the next loop of this opening
    m_anim.clear();
    if (at >= 0)
        m_grid->update(m_grid->tile(at));
}
