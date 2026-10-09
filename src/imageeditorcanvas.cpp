// 2.2 editor: ImageEditor's colours, glyphs, buttons and canvas (see imageeditor_p.h).

#include "imageeditor_p.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>

#include <cmath>

#include "i18n.h"
#include "uiutil.h"

using imageedit::Handle;
using imageedit::Shape;
using imageedit::ShapeType;

namespace {

constexpr int   kCanvasMargin = 24;   // around the fitted picture
constexpr int   kHandleHit    = 12;   // crop handles: hit squares
constexpr qreal kMaxZoom      = 8.0;  // device pixels per picture pixel
constexpr qreal kWheelStep    = 1.25; // per wheel notch with Ctrl
constexpr qreal kPenStep      = 1.5;  // logical px between pen points
constexpr qreal kMinShapePx   = 3.0;  // logical px: smaller rectangles and arrows are dropped
constexpr int   kMaxTextChars = 200;

} // namespace

// ============================================================================================
// Colours and glyphs
// ============================================================================================

namespace editorui {

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

Colors colorsFor(const QPalette& pal)
{
    Colors c;
    c.window = pal.color(QPalette::Active, QPalette::Window);
    c.text   = pal.color(QPalette::Active, QPalette::WindowText);
    c.dark   = c.window.lightness() < 128;
    c.muted  = ui::flatten(withAlpha(c.text, 175.0 / 255.0), c.window);
    if (ui::contrastRatio(c.muted, c.window) < 4.5)
        c.muted = c.text;
    c.accent = c.dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
    if (ui::contrastRatio(c.accent, c.window) < 3.0)
        c.accent = c.text;
    c.error = c.dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d);
    if (ui::contrastRatio(c.error, c.window) < 4.5)
        c.error = c.text;
    c.frame    = ui::flatten(withAlpha(c.text, 0.22), c.window);
    c.canvas   = c.dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0xeb, 0xed, 0xef);
    c.checkerA = c.dark ? QColor(0x3a, 0x3c, 0x42) : QColor(0xff, 0xff, 0xff);
    c.checkerB = c.dark ? QColor(0x2b, 0x2d, 0x31) : QColor(0xd9, 0xdc, 0xe0);
    return c;
}

void drawAlert(QPainter& p, const QRectF& box, const QColor& fill, const QColor& mark)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(box.adjusted(s * 0.04, s * 0.04, -s * 0.04, -s * 0.04));
    p.setPen(QPen(mark, s * 0.12, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx, box.top() + s * 0.26), QPointF(cx, box.top() + s * 0.56));
    p.setPen(Qt::NoPen);
    p.setBrush(mark);
    p.drawEllipse(QPointF(cx, box.top() + s * 0.74), s * 0.07, s * 0.07);
    p.restore();
}

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    const qreal s  = box.width();
    const auto  at = [&box, s](qreal x, qreal y) { return QPointF(box.left() + x * s, box.top() + y * s); };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, qMax(1.5, s * 0.095), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::Crop: {
        QPainterPath path;
        path.moveTo(at(0.30, 0.06));
        path.lineTo(at(0.30, 0.70));
        path.lineTo(at(0.94, 0.70));
        path.moveTo(at(0.06, 0.30));
        path.lineTo(at(0.70, 0.30));
        path.lineTo(at(0.70, 0.94));
        p.drawPath(path);
        break;
    }
    case Glyph::Pen: {
        // A pencil from the bottom left (its tip) to the top right.
        const QPointF tip = at(0.12, 0.88);
        const QPointF end = at(0.84, 0.16);
        const QPointF d   = (end - tip) / std::hypot(end.x() - tip.x(), end.y() - tip.y());
        const QPointF n(-d.y(), d.x());
        const qreal   w     = s * 0.13;
        const QPointF neck  = tip + d * (s * 0.24);
        const QPointF band  = end - d * (s * 0.22);
        QPolygonF     body;
        body << tip << neck + n * w << end + n * w << end - n * w << neck - n * w;
        p.drawPolygon(body);
        p.drawLine(neck + n * w, neck - n * w);
        p.drawLine(band + n * w, band - n * w);
        break;
    }
    case Glyph::Arrow:
        p.drawLine(at(0.16, 0.84), at(0.82, 0.18));
        p.drawLine(at(0.82, 0.18), at(0.44, 0.18));
        p.drawLine(at(0.82, 0.18), at(0.82, 0.56));
        break;
    case Glyph::Rect:
        p.drawRoundedRect(QRectF(at(0.10, 0.22), at(0.90, 0.78)), s * 0.06, s * 0.06);
        break;
    case Glyph::Text:
        p.drawLine(at(0.18, 0.18), at(0.82, 0.18));
        p.drawLine(at(0.50, 0.18), at(0.50, 0.86));
        p.drawLine(at(0.38, 0.86), at(0.62, 0.86));
        break;
    case Glyph::Hide: {
        // A mosaic: a frame with every other cell filled.
        const QRectF frame(at(0.10, 0.10), at(0.90, 0.90));
        p.drawRoundedRect(frame, s * 0.08, s * 0.08);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const qreal cell = frame.width() / 3.0;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                if ((row + col) % 2 == 0)
                    p.drawRect(QRectF(frame.left() + col * cell, frame.top() + row * cell, cell, cell).adjusted(s * 0.03, s * 0.03, -s * 0.03, -s * 0.03));
            }
        }
        break;
    }
    case Glyph::Undo:
    case Glyph::Redo: {
        const bool  mirror = glyph == Glyph::Redo;
        const auto  mx     = [mirror](qreal x) { return mirror ? 1.0 - x : x; };
        QPainterPath path;
        path.moveTo(at(mx(0.20), 0.38));
        path.lineTo(at(mx(0.60), 0.38));
        path.cubicTo(at(mx(0.92), 0.38), at(mx(0.92), 0.84), at(mx(0.60), 0.84));
        path.lineTo(at(mx(0.36), 0.84));
        p.drawPath(path);
        p.drawLine(at(mx(0.20), 0.38), at(mx(0.38), 0.20));
        p.drawLine(at(mx(0.20), 0.38), at(mx(0.38), 0.56));
        break;
    }
    case Glyph::Rotate: {
        // Clockwise: an open circle travelled clockwise, the arrow head at its end (upper right).
        const QRectF  circle(at(0.14, 0.16), at(0.86, 0.88));
        const qreal   end = 60.0;
        QPainterPath  path;
        path.arcMoveTo(circle, 0.0);
        path.arcTo(circle, 0.0, -300.0);
        p.drawPath(path);
        const qreal   rad = end * 3.14159265358979323846 / 180.0;
        const QPointF tip = path.currentPosition();
        const QPointF dir(std::sin(rad), std::cos(rad)); // the way a clockwise point moves there
        const QPointF n(-dir.y(), dir.x());
        const qreal   arm = s * 0.22;
        p.drawLine(tip, tip - dir * arm + n * arm * 0.85);
        p.drawLine(tip, tip - dir * arm - n * arm * 0.85);
        break;
    }
    case Glyph::Swap:
        p.drawRoundedRect(QRectF(at(0.08, 0.30), at(0.62, 0.70)), s * 0.05, s * 0.05);
        p.drawRoundedRect(QRectF(at(0.50, 0.08), at(0.92, 0.92)), s * 0.05, s * 0.05);
        break;
    case Glyph::None:
        break;
    }
    p.restore();
}

} // namespace editorui

using editorui::Colors;
using editorui::Glyph;
using editorui::withAlpha;

// ============================================================================================
// Button
// ============================================================================================

ImageEditor::Button::Button(Kind kind, QWidget* parent)
    : QAbstractButton(parent)
    , m_kind(kind)
{
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    m_side = kind == Kind::Glyph ? 32 : 28;
}

void ImageEditor::Button::setGlyph(Glyph glyph)
{
    m_glyph = glyph;
    updateGeometry();
    update();
}

void ImageEditor::Button::setSwatch(const QColor& color)
{
    m_swatch = color;
    update();
}

void ImageEditor::Button::setDot(qreal diameter)
{
    m_dot = diameter;
    update();
}

void ImageEditor::Button::setSide(int side)
{
    m_side = side;
    updateGeometry();
}

void ImageEditor::Button::setColors(const Colors& colors)
{
    m_colors = colors;
    update();
}

QSize ImageEditor::Button::sizeHint() const
{
    if (m_kind != Kind::Text)
        return QSize(m_side, m_side);
    const int textWidth = fontMetrics().horizontalAdvance(text());
    const int glyph     = m_glyph == Glyph::None ? 0 : 16 + 6;
    return QSize(textWidth + glyph + 20, 28);
}

void ImageEditor::Button::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const bool   enabled = isEnabled();

    if (m_kind == Kind::Swatch) {
        // A 20 px disc in a 28 px target; the chosen one gets an accent ring around it.
        const QPointF c = QRectF(rect()).center();
        if (isChecked()) {
            p.setPen(QPen(m_colors.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 12.5, 12.5);
        } else if (underMouse() && enabled) {
            p.setPen(QPen(withAlpha(m_colors.text, 0.35), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 12.5, 12.5);
        }
        QColor fill = m_swatch;
        if (!enabled)
            fill = withAlpha(fill, 0.35);
        p.setPen(QPen(m_colors.frame, 1.0)); // white on a light window, black on a dark one stay visible
        p.setBrush(fill);
        p.drawEllipse(c, 10, 10);
        if (hasFocus() && m_keyboardFocus) {
            p.setPen(QPen(m_colors.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 6, 6);
        }
        return;
    }

    QColor background(Qt::transparent);
    if (isChecked())
        background = withAlpha(m_colors.accent, isDown() ? 0.32 : 0.22);
    else if (isDown())
        background = withAlpha(m_colors.text, 0.18);
    else if (underMouse() && enabled)
        background = withAlpha(m_colors.text, 0.10);
    if (background.alpha() > 0) {
        p.setPen(isChecked() ? QPen(withAlpha(m_colors.accent, 0.9), 1.0) : Qt::NoPen);
        p.setBrush(background);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }
    const QColor ink = !enabled ? withAlpha(m_colors.text, 0.35) : isChecked() ? m_colors.accent : m_colors.text;

    if (m_kind == Kind::Dot) {
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QRectF(rect()).center(), m_dot / 2.0, m_dot / 2.0);
    } else if (m_kind == Kind::Glyph) {
        QRectF box(0, 0, 18, 18);
        box.moveCenter(QRectF(rect()).center());
        editorui::drawGlyph(p, m_glyph, box, ink);
    } else {
        qreal x = 10;
        if (m_glyph != Glyph::None) {
            editorui::drawGlyph(p, m_glyph, QRectF(x, (height() - 16) / 2.0, 16, 16), ink);
            x += 22;
        }
        QColor textColor = !enabled ? withAlpha(m_colors.text, 0.45) : m_colors.text;
        p.setPen(textColor);
        p.setFont(font());
        p.drawText(QRectF(x, 0, width() - x - 10, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
    if (hasFocus() && m_keyboardFocus) {
        p.setPen(QPen(m_colors.accent, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 6, 6);
    }
}

void ImageEditor::Button::focusInEvent(QFocusEvent* event)
{
    m_keyboardFocus = event->reason() != Qt::MouseFocusReason && event->reason() != Qt::PopupFocusReason;
    QAbstractButton::focusInEvent(event);
}

void ImageEditor::Button::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier) {
        click(); // like a focused push button: Enter acts on it, it doesn't finish
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

// ============================================================================================
// Canvas
// ============================================================================================

ImageEditor::Canvas::Canvas(ImageEditor* editor)
    : QWidget(editor)
    , m_editor(editor)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
    setAccessibleName(i18n::t("Picture"));
}

ImageEditor::Canvas::~Canvas() = default;

void ImageEditor::Canvas::setColors(const Colors& colors)
{
    m_colors = colors;
    if (m_text)
        layoutText();
    update();
}

void ImageEditor::Canvas::setMessage(const QString& text, bool error)
{
    m_message      = text;
    m_messageError = error;
    update();
}

void ImageEditor::Canvas::resetView()
{
    m_fit = true;
    m_mips.clear();
    closeText();
    m_hasLive = false;
    m_drag    = Drag::None;
    invalidate();
}

void ImageEditor::Canvas::invalidate()
{
    m_dirty = true;
    update();
}

void ImageEditor::Canvas::viewChanged()
{
    invalidate();
    if (m_text)
        layoutText();
    m_editor->updateStatus();
}

QRect ImageEditor::Canvas::region() const
{
    const imageedit::ImageEditModel& model = m_editor->m_model;
    return m_editor->m_tool == Tool::Crop ? QRect(QPoint(0, 0), model.rotatedSize()) : model.cropInRotated();
}

qreal ImageEditor::Canvas::fitScale() const
{
    const QRect r = region();
    if (r.isEmpty())
        return 1.0;
    const qreal availW = qMax(1, width() - 2 * kCanvasMargin);
    const qreal availH = qMax(1, height() - 2 * kCanvasMargin);
    // Never larger than actual size: small pictures stay pixel-exact.
    return qMin(qMin(availW / r.width(), availH / r.height()), 1.0 / devicePixelRatioF());
}

qreal ImageEditor::Canvas::scale() const
{
    return m_fit ? fitScale() : m_zoom / devicePixelRatioF();
}

qreal ImageEditor::Canvas::zoomFactor() const
{
    return scale() * devicePixelRatioF();
}

QTransform ImageEditor::Canvas::rotatedToWidget() const
{
    const QRect   r      = region();
    const qreal   s      = scale();
    const QPointF center = m_fit ? QRectF(r).center() : m_center;
    const QTransform t   = QTransform::fromTranslate(-center.x(), -center.y()) * QTransform::fromScale(s, s) * QTransform::fromTranslate(width() / 2.0, height() / 2.0);
    // The picture's corner on a whole device pixel: crisp at 100% and at any screen scaling.
    const qreal   dpr = devicePixelRatioF();
    const QPointF o   = t.map(QPointF(r.topLeft()));
    const QPointF snapped(std::round(o.x() * dpr) / dpr, std::round(o.y() * dpr) / dpr);
    return t * QTransform::fromTranslate(snapped.x() - o.x(), snapped.y() - o.y());
}

QTransform ImageEditor::Canvas::baseToWidget() const
{
    const imageedit::ImageEditModel& model = m_editor->m_model;
    return imageedit::rotationTransform(model.rotation(), model.baseSize()) * rotatedToWidget();
}

QTransform ImageEditor::Canvas::baseToDevice() const
{
    const qreal dpr = devicePixelRatioF();
    return baseToWidget() * QTransform::fromScale(dpr, dpr);
}

QPointF ImageEditor::Canvas::widgetToBase(const QPointF& pos) const
{
    return baseToWidget().inverted().map(pos);
}

QPointF ImageEditor::Canvas::widgetToRotated(const QPointF& pos) const
{
    return rotatedToWidget().inverted().map(pos);
}

QRectF ImageEditor::Canvas::cropOnScreen() const
{
    const imageedit::ImageEditModel& model = m_editor->m_model;
    const QRect rotated = imageedit::baseToRotated(m_editor->m_pendingCrop, model.rotation(), model.baseSize());
    return rotatedToWidget().mapRect(QRectF(rotated));
}

Handle ImageEditor::Canvas::handleAt(const QPointF& pos) const
{
    const QRectF c    = cropOnScreen();
    const qreal  half = kHandleHit / 2.0;
    const auto   near = [half](qreal a, qreal b) { return std::abs(a - b) <= half; };
    const bool   inX  = pos.x() >= c.left() - half && pos.x() <= c.right() + half;
    const bool   inY  = pos.y() >= c.top() - half && pos.y() <= c.bottom() + half;
    if (!inX || !inY)
        return Handle::None;
    const bool left = near(pos.x(), c.left()), right = near(pos.x(), c.right());
    const bool top = near(pos.y(), c.top()), bottom = near(pos.y(), c.bottom());
    if (top && left)
        return Handle::TopLeft;
    if (top && right)
        return Handle::TopRight;
    if (bottom && left)
        return Handle::BottomLeft;
    if (bottom && right)
        return Handle::BottomRight;
    if (left)
        return Handle::Left;
    if (right)
        return Handle::Right;
    if (top)
        return Handle::Top;
    if (bottom)
        return Handle::Bottom;
    return c.contains(pos) ? Handle::Move : Handle::None;
}

void ImageEditor::Canvas::updateCursor(const QPointF& pos)
{
    if (!m_editor->m_loaded || m_editor->m_saving) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (m_drag == Drag::Pan) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_space) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    switch (m_editor->m_tool) {
    case Tool::Crop:
        switch (m_drag == Drag::Crop ? m_handle : handleAt(pos)) {
        case Handle::TopLeft:
        case Handle::BottomRight:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case Handle::TopRight:
        case Handle::BottomLeft:
            setCursor(Qt::SizeBDiagCursor);
            break;
        case Handle::Left:
        case Handle::Right:
            setCursor(Qt::SizeHorCursor);
            break;
        case Handle::Top:
        case Handle::Bottom:
            setCursor(Qt::SizeVerCursor);
            break;
        case Handle::Move:
            setCursor(Qt::SizeAllCursor);
            break;
        case Handle::None:
            setCursor(Qt::CrossCursor);
            break;
        }
        break;
    case Tool::Text:
        setCursor(Qt::IBeamCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
        break;
    }
}

void ImageEditor::Canvas::leaveFit()
{
    if (!m_fit)
        return;
    m_zoom   = zoomFactor();
    m_center = QRectF(region()).center();
    m_fit    = false;
}

void ImageEditor::Canvas::clampCenter()
{
    if (m_fit)
        return;
    const QRectF r     = QRectF(region());
    const qreal  s     = scale();
    const qreal  halfW = width() / 2.0 / s;
    const qreal  halfH = height() / 2.0 / s;
    m_center.setX(r.width() <= 2 * halfW ? r.center().x() : qBound(r.left() + halfW, m_center.x(), r.right() - halfW));
    m_center.setY(r.height() <= 2 * halfH ? r.center().y() : qBound(r.top() + halfH, m_center.y(), r.bottom() - halfH));
}

void ImageEditor::Canvas::zoomAt(qreal factor, const QPointF& anchor)
{
    if (m_editor->m_model.baseSize().isEmpty() || factor <= 0)
        return;
    const qreal   dpr     = devicePixelRatioF();
    const qreal   minZoom = fitScale() * dpr;
    const qreal   zoom    = qBound(minZoom, zoomFactor() * factor, qMax(minZoom, kMaxZoom));
    const QPointF under   = widgetToRotated(anchor);
    if (zoom <= minZoom * 1.0001) {
        m_fit = true;
    } else {
        m_fit    = false;
        m_zoom   = zoom;
        // The point under the pointer stays under it.
        m_center = under - (anchor - QPointF(width() / 2.0, height() / 2.0)) / (zoom / dpr);
        clampCenter();
    }
    viewChanged();
}

void ImageEditor::Canvas::fit()
{
    m_fit = true;
    viewChanged();
}

void ImageEditor::Canvas::actualSize()
{
    zoomAt(1.0 / zoomFactor(), QPointF(width() / 2.0, height() / 2.0));
}

void ImageEditor::Canvas::toolChanged()
{
    if (m_editor->m_tool != Tool::Text)
        commitText();
    m_hasLive = false;
    m_drag    = Drag::None;
    clampCenter();
    viewChanged();
    updateCursor(mapFromGlobal(QCursor::pos()));
}

// ---- the picture at the current view ------------------------------------------------------------

const QImage* ImageEditor::Canvas::sourceFor(qreal deviceScale)
{
    const QImage& base = m_editor->m_model.base();
    if (base.isNull() || deviceScale >= 0.5)
        return nullptr; // the picture itself
    // The smallest halving that is still at least as large as the screen needs: smooth and fast.
    int level = 0;
    while (std::pow(0.5, level + 1) >= deviceScale && level < 12)
        ++level;
    if (m_mips.isEmpty())
        m_mips.append(QImage()); // level 0 is the picture
    while (m_mips.size() <= level) {
        const QImage& previous = m_mips.size() == 1 ? base : m_mips.last();
        if (previous.width() < 4 || previous.height() < 4)
            break;
        m_mips.append(previous.scaled(qMax(1, previous.width() / 2), qMax(1, previous.height() / 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
    level = qMin(level, m_mips.size() - 1);
    return level > 0 ? &m_mips.at(level) : nullptr;
}

void ImageEditor::Canvas::rebuildComposite()
{
    const qreal dpr  = devicePixelRatioF();
    const QSize size = (QSizeF(this->size()) * dpr).toSize().expandedTo(QSize(1, 1));
    if (m_composite.size() != size)
        m_composite = QImage(size, QImage::Format_ARGB32_Premultiplied);
    m_composite.fill(Qt::transparent);
    const QImage* source = sourceFor(zoomFactor());
    m_editor->m_model.render(m_composite, baseToDevice(), source);
    m_dirty = false;
}

void ImageEditor::Canvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), m_colors.canvas);
    const imageedit::ImageEditModel& model = m_editor->m_model;
    if (!m_message.isEmpty() || model.base().isNull()) {
        QFont font = this->font();
        if (font.pixelSize() > 0)
            font.setPixelSize(qRound(font.pixelSize() * 1.15));
        else
            font.setPointSizeF((font.pointSizeF() > 0 ? font.pointSizeF() : 9.0) * 1.15);
        p.setFont(font);
        const QFontMetrics fm(font);
        const QString      text  = m_message.isEmpty() ? i18n::t("Opening the picture…") : m_message;
        const int          textW = fm.horizontalAdvance(text);
        const int          icon  = m_messageError ? 18 : 0;
        const int          total = textW + (icon ? icon + 8 : 0);
        qreal              x     = (width() - total) / 2.0;
        const qreal        y     = height() / 2.0 - (m_messageError ? fm.height() / 2.0 : 0);
        const QColor       ink   = m_colors.dark ? QColor(0xdb, 0xde, 0xe1) : QColor(0x31, 0x33, 0x38);
        if (icon) {
            editorui::drawAlert(p, QRectF(x, y - icon / 2.0, icon, icon), m_colors.dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d), m_colors.canvas);
            x += icon + 8;
        }
        p.setPen(ink);
        p.drawText(QRectF(x, y - fm.height() / 2.0, textW + 2, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
        if (m_messageError) {
            // The way on: the picture can still be sent as it is.
            const QColor muted = ui::flatten(withAlpha(ink, 175.0 / 255.0), m_colors.canvas);
            p.setPen(ui::contrastRatio(muted, m_colors.canvas) >= 4.5 ? muted : ink);
            p.setFont(this->font());
            p.drawText(QRectF(0, y + fm.height() * 0.75, width(), fm.height() * 1.5), Qt::AlignHCenter | Qt::AlignTop,
                       i18n::t("Cancel to go back; the file can still be sent as it is."));
        }
        return;
    }
    const qreal dpr = devicePixelRatioF();
    if (m_dirty || m_composite.size() != (QSizeF(size()) * dpr).toSize())
        rebuildComposite();

    const QRectF picture = rotatedToWidget().mapRect(QRectF(region()));
    p.save();
    p.setClipRect(picture);
    if (model.base().hasAlphaChannel()) {
        // A checkerboard behind transparent parts (8 px squares), anchored at the picture's corner.
        QPixmap tile(16, 16);
        tile.fill(m_colors.checkerA);
        {
            QPainter t(&tile);
            t.fillRect(8, 0, 8, 8, m_colors.checkerB);
            t.fillRect(0, 8, 8, 8, m_colors.checkerB);
        }
        p.setBrushOrigin(picture.topLeft());
        p.fillRect(picture, QBrush(tile));
        p.setBrushOrigin(QPointF());
    }
    p.drawImage(QRectF(rect()), m_composite, QRectF(m_composite.rect()));
    if (m_hasLive)
        drawLive(p);
    p.restore();

    if (m_editor->m_tool != Tool::Crop)
        return;
    // The crop: outside dimmed (black at 60%), a frame, the thirds while dragging, 8 handles.
    const QRectF crop = cropOnScreen();
    QPainterPath outside;
    outside.addRect(picture);
    QPainterPath inside;
    inside.addRect(crop);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 153));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(crop);
    p.setPen(QPen(Qt::white, 1.0));
    p.drawRect(crop);
    if (m_drag == Drag::Crop || m_drag == Drag::NewCrop) {
        p.setPen(QPen(QColor(255, 255, 255, 150), 1.0));
        for (int i = 1; i <= 2; ++i) {
            const qreal x = crop.left() + crop.width() * i / 3.0;
            const qreal y = crop.top() + crop.height() * i / 3.0;
            p.drawLine(QPointF(x, crop.top()), QPointF(x, crop.bottom()));
            p.drawLine(QPointF(crop.left(), y), QPointF(crop.right(), y));
        }
    }
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF handles[] = {crop.topLeft(), crop.topRight(), crop.bottomLeft(), crop.bottomRight(),
                               QPointF(crop.center().x(), crop.top()), QPointF(crop.center().x(), crop.bottom()),
                               QPointF(crop.left(), crop.center().y()), QPointF(crop.right(), crop.center().y())};
    p.setPen(QPen(QColor(0, 0, 0, 170), 1.0));
    p.setBrush(Qt::white);
    for (const QPointF& h : handles)
        p.drawRoundedRect(QRectF(h.x() - 4.5, h.y() - 4.5, 9, 9), 2, 2);
}

void ImageEditor::Canvas::drawLive(QPainter& p)
{
    const qreal  dpr   = devicePixelRatioF();
    const QRectF onScreen = baseToWidget().mapRect(m_live.rect);
    if (m_live.type == ShapeType::Pixelate || m_live.type == ShapeType::BlackBox) {
        const QTransform toDevice = baseToDevice();
        const QRect      device   = imageedit::pixelsInside(toDevice.mapRect(m_live.rect)) & m_composite.rect();
        if (!device.isEmpty()) {
            QImage part = m_composite.copy(device);
            const QTransform toPart = toDevice * QTransform::fromTranslate(-device.x(), -device.y());
            if (m_live.type == ShapeType::Pixelate) // the same seed render() will use for it
                imageedit::pixelate(part, toPart, m_live.rect, 0x9E3779B9U * static_cast<quint32>(m_editor->m_model.shapes().size() + 1));
            else
                imageedit::blackBox(part, toPart, m_live.rect);
            p.drawImage(QRectF(QPointF(device.topLeft()) / dpr, QSizeF(device.size()) / dpr), part);
        }
        QPen outline(m_colors.dark ? m_colors.accent : QColor(0x58, 0x65, 0xf2), 1.0, Qt::DashLine);
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        p.drawRect(onScreen);
        return;
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(baseToWidget(), true);
    imageedit::drawShape(p, m_live);
    p.restore();
}

// ---- input ------------------------------------------------------------------------------------------

void ImageEditor::Canvas::mousePressEvent(QMouseEvent* event)
{
    if (!m_editor->m_loaded || m_editor->m_saving) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const QPointF pos = event->localPos();
    if (m_text) {
        // A click elsewhere finishes the text being typed (and does nothing else).
        commitText();
        return;
    }
    if (m_space || event->button() == Qt::MiddleButton) {
        leaveFit();
        m_drag        = Drag::Pan;
        m_pressPos    = pos;
        m_pressCenter = m_center;
        updateCursor(pos);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_pressPos = pos;
    switch (m_editor->m_tool) {
    case Tool::Crop: {
        const imageedit::ImageEditModel& model = m_editor->m_model;
        m_cropStartBase = m_editor->m_pendingCrop;
        m_cropStart     = imageedit::baseToRotated(m_editor->m_pendingCrop, model.rotation(), model.baseSize());
        m_handle        = handleAt(pos);
        m_drag          = m_handle == Handle::None ? Drag::NewCrop : Drag::Crop;
        update();
        break;
    }
    case Tool::Text:
        beginText(pos);
        break;
    default: {
        m_drag      = Drag::Draw;
        m_liveStart = widgetToBase(pos);
        m_live      = Shape();
        const qreal s = scale();
        switch (m_editor->m_tool) {
        case Tool::Pen:
            m_live.type   = ShapeType::Pen;
            m_live.points = {m_liveStart};
            break;
        case Tool::Arrow:
            m_live.type   = ShapeType::Arrow;
            m_live.points = {m_liveStart, m_liveStart};
            break;
        case Tool::Rect:
            m_live.type = ShapeType::Rect;
            m_live.rect = QRectF(m_liveStart, QSizeF());
            break;
        default:
            m_live.type = m_editor->m_hideMode == 1 ? ShapeType::BlackBox : ShapeType::Pixelate;
            m_live.rect = QRectF(m_liveStart, QSizeF());
            break;
        }
        m_live.color = imageedit::paletteColor(m_editor->m_color);
        m_live.width = imageedit::strokeLogicalPx(m_editor->m_stroke) / s; // looks the same at any zoom, exact in the file
        m_hasLive    = true;
        update();
        break;
    }
    }
}

void ImageEditor::Canvas::updateLive(const QPointF& pos, bool shift)
{
    const QPointF b = widgetToBase(pos);
    switch (m_live.type) {
    case ShapeType::Pen:
        if (shift) {
            m_live.points = {m_live.points.first(), b}; // a straight line
        } else {
            const QPointF last = m_live.points.last();
            if (std::hypot(b.x() - last.x(), b.y() - last.y()) * scale() >= kPenStep)
                m_live.points.append(b);
        }
        break;
    case ShapeType::Arrow:
        m_live.points = {m_liveStart, shift ? imageedit::snap45(m_liveStart, b) : b};
        break;
    case ShapeType::Rect:
        m_live.rect = shift ? imageedit::squareFrom(m_liveStart, b) : QRectF(m_liveStart, b).normalized();
        break;
    default: {
        // Hiding only makes sense on the picture.
        const QRectF whole(QPointF(0, 0), QSizeF(m_editor->m_model.baseSize()));
        m_live.rect = (shift ? imageedit::squareFrom(m_liveStart, b) : QRectF(m_liveStart, b).normalized()) & whole;
        break;
    }
    }
    update();
}

void ImageEditor::Canvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->localPos();
    switch (m_drag) {
    case Drag::Pan:
        m_center = m_pressCenter - (pos - m_pressPos) / scale();
        clampCenter();
        viewChanged();
        return;
    case Drag::NewCrop:
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        {
            // A new crop from where the press was, towards the pointer.
            const QPointF start = widgetToRotated(m_pressPos);
            m_cropStart         = QRect(QPoint(qRound(start.x()), qRound(start.y())), QSize(0, 0));
            const bool left     = pos.x() < m_pressPos.x();
            const bool up       = pos.y() < m_pressPos.y();
            m_handle            = left ? (up ? Handle::TopLeft : Handle::BottomLeft) : (up ? Handle::TopRight : Handle::BottomRight);
            m_drag              = Drag::Crop;
        }
        Q_FALLTHROUGH();
    case Drag::Crop: {
        const imageedit::ImageEditModel& model = m_editor->m_model;
        const QRect                      bounds(QPoint(0, 0), model.rotatedSize());
        const QRect r = imageedit::dragCrop(m_cropStart, m_handle, (pos - m_pressPos) / scale(), bounds, m_editor->aspectValue(), imageedit::kMinCropSide);
        if (!r.isEmpty())
            m_editor->m_pendingCrop = imageedit::rotatedToBase(r, model.rotation(), model.baseSize());
        update();
        m_editor->updateStatus();
        return;
    }
    case Drag::Draw:
        updateLive(pos, event->modifiers() & Qt::ShiftModifier);
        return;
    case Drag::None:
        break;
    }
    updateCursor(pos);
}

void ImageEditor::Canvas::finishDraw()
{
    m_drag = Drag::None;
    if (!m_hasLive)
        return;
    m_hasLive      = false;
    const qreal s  = scale();
    bool        ok = true;
    switch (m_live.type) {
    case ShapeType::Pen:
        break; // a click leaves a dot
    case ShapeType::Arrow: {
        const QPointF d = m_live.points.value(1) - m_live.points.value(0);
        ok              = std::hypot(d.x(), d.y()) * s >= kMinShapePx * 2;
        break;
    }
    default:
        ok = m_live.rect.width() * s >= kMinShapePx && m_live.rect.height() * s >= kMinShapePx;
        break;
    }
    if (ok) {
        m_editor->m_model.addShape(m_live);
        m_editor->modelChanged();
    } else {
        update();
    }
}

void ImageEditor::Canvas::mouseReleaseEvent(QMouseEvent* event)
{
    switch (m_drag) {
    case Drag::Pan:
        m_drag = Drag::None;
        break;
    case Drag::Crop:
    case Drag::NewCrop:
        m_drag = Drag::None;
        update();
        m_editor->updateControls();
        m_editor->updateStatus();
        break;
    case Drag::Draw:
        if (event->button() == Qt::LeftButton)
            finishDraw();
        break;
    case Drag::None:
        break;
    }
    updateCursor(event->localPos());
}

void ImageEditor::Canvas::wheelEvent(QWheelEvent* event)
{
    if (!m_editor->m_loaded) {
        event->ignore();
        return;
    }
    const QPoint angle = event->angleDelta();
    if (event->modifiers() & Qt::ControlModifier) {
        if (angle.y() != 0)
            zoomAt(std::pow(kWheelStep, angle.y() / 120.0), event->position());
        event->accept();
        return;
    }
    if (m_fit) {
        event->ignore();
        return;
    }
    // Scrolls the zoomed picture (Shift: sideways).
    const qreal   s     = scale();
    const bool    side  = event->modifiers() & Qt::ShiftModifier;
    const QPointF delta = side ? QPointF(angle.y(), 0) : QPointF(angle.x(), angle.y());
    m_center -= delta / 2.0 / s;
    clampCenter();
    viewChanged();
    event->accept();
}

void ImageEditor::Canvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        if (!event->isAutoRepeat()) {
            m_space = true;
            updateCursor(mapFromGlobal(QCursor::pos()));
        }
        event->accept();
        return;
    }
    event->ignore(); // the editor's shortcuts
}

void ImageEditor::Canvas::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_space = false;
        updateCursor(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    event->ignore();
}

void ImageEditor::Canvas::focusOutEvent(QFocusEvent* event)
{
    m_space = false;
    QWidget::focusOutEvent(event);
}

void ImageEditor::Canvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    clampCenter();
    m_dirty = true;
    if (m_text)
        layoutText();
}

bool ImageEditor::Canvas::cancelInteraction()
{
    if (m_text) {
        closeText();
        m_editor->updateStatus();
        return true;
    }
    switch (m_drag) {
    case Drag::Draw:
        m_hasLive = false;
        m_drag    = Drag::None;
        update();
        return true;
    case Drag::Crop:
    case Drag::NewCrop:
        m_editor->m_pendingCrop = m_cropStartBase;
        m_drag                  = Drag::None;
        update();
        m_editor->updateStatus();
        return true;
    case Drag::Pan:
        m_drag = Drag::None;
        return true;
    case Drag::None:
        break;
    }
    return false;
}

// ---- text ---------------------------------------------------------------------------------------------

void ImageEditor::Canvas::beginText(const QPointF& pos)
{
    m_textAnchor   = widgetToBase(pos);
    m_textFontPx   = imageedit::textLogicalPx(m_editor->m_stroke) / scale();
    m_textRotation = m_editor->m_model.rotation();
    auto* edit     = new QLineEdit(this);
    edit->setFrame(false);
    edit->setMaxLength(kMaxTextChars);
    edit->setTextMargins(0, 0, 0, 0);
    edit->setAccessibleName(i18n::t("Text to add"));
    edit->installEventFilter(this);
    connect(edit, &QLineEdit::textChanged, this, [this] { layoutText(); });
    m_text = edit;
    layoutText();
    edit->show();
    edit->setFocus(Qt::OtherFocusReason);
    m_editor->updateStatus();
}

// The text box sits where the text will be drawn: its text's top left corner on the anchor, at the
// size and in the colour it will have.
void ImageEditor::Canvas::layoutText()
{
    if (!m_text)
        return;
    QFont font(QString::fromLatin1("Segoe UI"));
    font.setWeight(QFont::DemiBold);
    font.setPixelSize(qMax(6, qRound(m_textFontPx * scale())));
    m_text->setFont(font);
    const QColor color = imageedit::paletteColor(m_editor->m_color);
    // fromLatin1: TeamSpeak's style keeps parsed style sheets after the plugin is unloaded.
    m_text->setStyleSheet(QString::fromLatin1("QLineEdit{background:transparent;border:1px dashed %1;padding:0px;margin:0px;color:%2;"
                                              "selection-background-color:%1;selection-color:#ffffff;}")
                              .arg(m_colors.accent.name(), color.name()));
    const QFontMetrics fm(font);
    const int          w = qMax(fm.horizontalAdvance(QLatin1Char('M')) * 2, fm.horizontalAdvance(m_text->text()) + fm.horizontalAdvance(QLatin1Char('M')));
    m_text->resize(w + 8, fm.height() + 2);
    const QPointF at = baseToWidget().map(m_textAnchor);
    m_text->move(qRound(at.x()) - 3, qRound(at.y()) - 1);
}

void ImageEditor::Canvas::styleChanged()
{
    if (!m_text)
        return;
    m_textFontPx = imageedit::textLogicalPx(m_editor->m_stroke) / scale();
    layoutText();
}

bool ImageEditor::Canvas::hasText() const
{
    return m_text && !m_text->text().trimmed().isEmpty();
}

void ImageEditor::Canvas::closeText()
{
    if (!m_text)
        return;
    QLineEdit* edit = m_text;
    m_text          = nullptr;
    edit->removeEventFilter(this);
    edit->hide();
    edit->deleteLater();
    if (isVisible())
        setFocus(Qt::OtherFocusReason);
}

void ImageEditor::Canvas::commitText()
{
    if (!m_text)
        return;
    const QString text = m_text->text();
    closeText();
    if (text.trimmed().isEmpty()) {
        m_editor->updateStatus();
        return;
    }
    Shape shape;
    shape.type         = ShapeType::Text;
    shape.color        = imageedit::paletteColor(m_editor->m_color);
    shape.text         = text;
    shape.fontPx       = m_textFontPx;
    shape.rect         = QRectF(m_textAnchor, QSizeF());
    shape.textRotation = m_textRotation;
    m_editor->m_model.addShape(shape);
    m_editor->modelChanged();
}

bool ImageEditor::Canvas::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_text && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            commitText(); // never Done: Enter only finishes the text
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            closeText();
            m_editor->updateStatus();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
