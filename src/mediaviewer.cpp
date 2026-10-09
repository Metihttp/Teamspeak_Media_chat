#include "mediaviewer.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QBasicTimer>
#include <QBoxLayout>
#include <QClipboard>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QThreadPool>
#include <QTimer>
#include <QTimerEvent>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QVector>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>
#include <functional>
#include <limits>

#include "core.h"
#include "filedrag.h" // 2.2 drag-out
#include "i18n.h"
#include "previewrenderer.h"
#include "settings.h"
#include "uiutil.h"
#include "version.h"
#include "video/mfvideo.h"

namespace {

constexpr qint64 kMaxPixels      = 80LL * 1000 * 1000; // never decode more than this
constexpr int    kHideControlsMs = 2500;
constexpr int    kFadeOutMs      = 200; // auto-hidden controls fade out; showing them again is instant
constexpr int    kDownloadDelay  = 250; // flipping quickly through the gallery does not start downloads
constexpr qreal  kCenterButton   = 34;  // radius of the big play button over a video

// ImageCanvas zoom, in device pixels per image pixel.
constexpr qreal  kMinZoom             = 0.05;
constexpr qreal  kMaxZoom             = 16.0;
constexpr qreal  kSmoothBelow         = 0.75; // smaller than this, bilinear sampling skips source pixels: draw a resampled copy
constexpr int    kRescaleDelayMs      = 120;  // the resampled copy follows once zooming stops
constexpr qint64 kRescaleInlinePixels = 24LL * 1000 * 1000; // larger pictures are resampled on a worker thread

// Windows virtual keys for shortcuts that don't depend on the keyboard layout.
constexpr quint32 kVkAdd      = 0x6B; // number pad +
constexpr quint32 kVkSubtract = 0x6D; // number pad -
constexpr quint32 kVkOemPlus  = 0xBB; // the = / + key
constexpr quint32 kVkOemMinus = 0xBD; // the - key
constexpr quint32 kVkOem2     = 0xBF; // the / ? key on US layouts

void useDarkTitleBar(QWidget* window); // Windows only, defined at the end of this file

// Mouse use hands the keys back to the window: after a click, Space, the arrows and the letter
// shortcuts act on the media again, not on a button that was reached with Tab before.
void focusWindow(QWidget* widget)
{
    if (QWidget* window = widget->window())
        window->setFocus(Qt::MouseFocusReason);
}

// Icon-only controls: screen readers announce the accessible name (a tooltip is only read as the
// description). Both change with the control's state, so only real changes are applied.
void setIconAction(QWidget* widget, const QString& name, const QString& tip)
{
    if (widget->accessibleName() != name)
        widget->setAccessibleName(name);
    if (widget->toolTip() != tip)
        widget->setToolTip(tip);
}

// The window's main action is drawn in the accent colour (style sheet: QPushButton#primary).
void setPrimary(QPushButton* button, bool primary)
{
    const QString name = primary ? QString::fromLatin1("primary") : QString();
    if (button->objectName() == name)
        return;
    button->setObjectName(name);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
}

// The Microsoft Store extension a playback error asks for ("Install “HEVC Video Extensions” from the
// Microsoft Store."), empty if none. mf::VideoPlayer reports a sentence only.
QString storeAppIn(const QString& error)
{
    if (!error.contains(QLatin1String("Microsoft Store")))
        return {};
    const int open  = error.indexOf(QChar(0x201C));
    const int close = open < 0 ? -1 : error.indexOf(QChar(0x201D), open + 1);
    return close > open + 1 ? error.mid(open + 1, close - open - 1) : QString();
}

// Whether a playback error already names its fix (a Store extension, the Media Feature Pack). Other
// apps use the same Windows decoders, so "open it with your default app" would not help then.
bool namesFix(const QString& error)
{
    return error.contains(QLatin1String("Microsoft Store")) || error.contains(QLatin1String("Media Feature Pack"));
}

// ---- icons ---------------------------------------------------------------------------------

enum class Glyph {
    Play,
    Pause,
    Replay,
    VolumeHigh,
    VolumeLow,
    VolumeMuted,
    Loop,
    FullscreenEnter,
    FullscreenExit,
    ChevronLeft,
    ChevronRight,
    Error,
    Info,
    Help,
    File,
    Music,
    Check,
    Copy,
    Save,
    Open,
};

// Draws a glyph designed on a 24x24 grid into box.
void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 24.0, box.height() / 24.0);
    const QPen stroke(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    auto speaker = [&] {
        QPainterPath body;
        body.moveTo(3.5, 9);
        body.lineTo(7.5, 9);
        body.lineTo(12, 5);
        body.lineTo(12, 19);
        body.lineTo(7.5, 15);
        body.lineTo(3.5, 15);
        body.closeSubpath();
        p.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(body);
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
    };

    switch (glyph) {
    case Glyph::Play: {
        QPainterPath path;
        path.moveTo(8, 5.5);
        path.lineTo(18.5, 12);
        path.lineTo(8, 18.5);
        path.closeSubpath();
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(path);
        break;
    }
    case Glyph::Pause:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(6.5, 5, 4, 14), 1, 1);
        p.drawRoundedRect(QRectF(13.5, 5, 4, 14), 1, 1);
        break;
    case Glyph::Replay: {
        const QRectF circle(5, 6, 14, 14);
        QPainterPath arc;
        arc.arcMoveTo(circle, 90);
        arc.arcTo(circle, 90, -290);
        p.drawPath(arc);
        QPainterPath head;
        head.moveTo(8.2, 6);
        head.lineTo(12.6, 2.8);
        head.lineTo(12.6, 9.2);
        head.closeSubpath();
        p.setPen(QPen(color, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(head);
        break;
    }
    case Glyph::VolumeHigh:
        speaker();
        p.drawArc(QRectF(8, 8, 8, 8), -50 * 16, 100 * 16);
        p.drawArc(QRectF(4.5, 4.5, 15, 15), -50 * 16, 100 * 16);
        break;
    case Glyph::VolumeLow:
        speaker();
        p.drawArc(QRectF(8, 8, 8, 8), -50 * 16, 100 * 16);
        break;
    case Glyph::VolumeMuted:
        speaker();
        p.drawLine(QPointF(15.5, 9.5), QPointF(20.5, 14.5));
        p.drawLine(QPointF(20.5, 9.5), QPointF(15.5, 14.5));
        break;
    case Glyph::Loop: {
        QPainterPath path;
        path.moveTo(4, 12.5);
        path.lineTo(4, 10);
        path.quadTo(4, 7, 7, 7);
        path.lineTo(18.5, 7);
        path.moveTo(15.5, 4);
        path.lineTo(18.5, 7);
        path.lineTo(15.5, 10);
        path.moveTo(20, 11.5);
        path.lineTo(20, 14);
        path.quadTo(20, 17, 17, 17);
        path.lineTo(5.5, 17);
        path.moveTo(8.5, 14);
        path.lineTo(5.5, 17);
        path.lineTo(8.5, 20);
        p.drawPath(path);
        break;
    }
    case Glyph::FullscreenEnter: {
        QPainterPath path;
        path.moveTo(4, 9);
        path.lineTo(4, 4);
        path.lineTo(9, 4);
        path.moveTo(15, 4);
        path.lineTo(20, 4);
        path.lineTo(20, 9);
        path.moveTo(20, 15);
        path.lineTo(20, 20);
        path.lineTo(15, 20);
        path.moveTo(9, 20);
        path.lineTo(4, 20);
        path.lineTo(4, 15);
        p.drawPath(path);
        break;
    }
    case Glyph::FullscreenExit: {
        QPainterPath path;
        path.moveTo(4, 9);
        path.lineTo(9, 9);
        path.lineTo(9, 4);
        path.moveTo(15, 4);
        path.lineTo(15, 9);
        path.lineTo(20, 9);
        path.moveTo(20, 15);
        path.lineTo(15, 15);
        path.lineTo(15, 20);
        path.moveTo(9, 20);
        path.lineTo(9, 15);
        path.lineTo(4, 15);
        p.drawPath(path);
        break;
    }
    case Glyph::ChevronLeft:
    case Glyph::ChevronRight: {
        p.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        if (glyph == Glyph::ChevronLeft) {
            path.moveTo(14.5, 6);
            path.lineTo(8.5, 12);
            path.lineTo(14.5, 18);
        } else {
            path.moveTo(9.5, 6);
            path.lineTo(15.5, 12);
            path.lineTo(9.5, 18);
        }
        p.drawPath(path);
        break;
    }
    case Glyph::Error:
    case Glyph::Info:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        if (glyph == Glyph::Error) {
            p.drawLine(QPointF(12, 7.5), QPointF(12, 13));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(12, 16.5), 1.3, 1.3);
        } else {
            p.drawLine(QPointF(12, 11), QPointF(12, 16.5));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(12, 7.5), 1.3, 1.3);
        }
        break;
    case Glyph::Help: {
        p.drawEllipse(QPointF(12, 12), 9, 9);
        QPainterPath hook;
        hook.moveTo(9.4, 9.6);
        hook.cubicTo(9.4, 7.9, 10.6, 6.9, 12, 6.9);
        hook.cubicTo(13.5, 6.9, 14.6, 7.9, 14.6, 9.3);
        hook.cubicTo(14.6, 11.2, 12, 11.4, 12, 13.6);
        p.drawPath(hook);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(12, 16.9), 1.3, 1.3);
        break;
    }
    case Glyph::Check: {
        QPainterPath path;
        path.moveTo(5, 12.5);
        path.lineTo(10, 17.5);
        path.lineTo(19, 7);
        p.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        break;
    }
    case Glyph::Copy: {
        p.drawRoundedRect(QRectF(9, 9, 11, 11), 2, 2);
        QPainterPath back; // the part of the back sheet that the front one doesn't cover
        back.moveTo(9, 15);
        back.lineTo(6, 15);
        back.quadTo(4, 15, 4, 13);
        back.lineTo(4, 6);
        back.quadTo(4, 4, 6, 4);
        back.lineTo(13, 4);
        back.quadTo(15, 4, 15, 6);
        back.lineTo(15, 9);
        p.drawPath(back);
        break;
    }
    case Glyph::Save: {
        QPainterPath path;
        path.moveTo(12, 4);
        path.lineTo(12, 15);
        path.moveTo(7.5, 10.5);
        path.lineTo(12, 15);
        path.lineTo(16.5, 10.5);
        path.moveTo(4, 15);
        path.lineTo(4, 18);
        path.quadTo(4, 20, 6, 20);
        path.lineTo(18, 20);
        path.quadTo(20, 20, 20, 18);
        path.lineTo(20, 15);
        p.drawPath(path);
        break;
    }
    case Glyph::Open: {
        QPainterPath path;
        path.moveTo(11, 5);
        path.lineTo(7, 5);
        path.quadTo(5, 5, 5, 7);
        path.lineTo(5, 17);
        path.quadTo(5, 19, 7, 19);
        path.lineTo(17, 19);
        path.quadTo(19, 19, 19, 17);
        path.lineTo(19, 13);
        path.moveTo(14, 5);
        path.lineTo(19, 5);
        path.lineTo(19, 10);
        path.moveTo(19, 5);
        path.lineTo(11.5, 12.5);
        p.drawPath(path);
        break;
    }
    case Glyph::File: {
        QPainterPath path;
        path.moveTo(6, 3);
        path.lineTo(14, 3);
        path.lineTo(19, 8);
        path.lineTo(19, 21);
        path.lineTo(6, 21);
        path.closeSubpath();
        path.moveTo(14, 3);
        path.lineTo(14, 8);
        path.lineTo(19, 8);
        p.drawPath(path);
        break;
    }
    case Glyph::Music: {
        p.drawLine(QPointF(12, 17), QPointF(12, 4.5));
        QPainterPath flag;
        flag.moveTo(12, 4.5);
        flag.quadTo(18, 5, 18, 10);
        p.drawPath(flag);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(9, 17.5), 3.2, 2.5);
        break;
    }
    }
    p.restore();
}

// Flat icon button (control bar) or round translucent button (gallery arrows, exit full screen).
class GlyphButton : public QAbstractButton
{
  public:
    GlyphButton(Glyph glyph, int size, bool round, QWidget* parent)
        : QAbstractButton(parent)
        , m_glyph(glyph)
        , m_round(round)
    {
        setFixedSize(size, size);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

    void setGlyph(Glyph glyph)
    {
        if (glyph == m_glyph)
            return;
        m_glyph = glyph;
        update();
    }

  protected:
    void enterEvent(QEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

    void mousePressEvent(QMouseEvent* event) override
    {
        focusWindow(this);
        QAbstractButton::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter     p(this);
        const bool   hover   = underMouse() && isEnabled();
        const bool   checked = isCheckable() && isChecked();
        const QRectF box(rect());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        if (m_round) {
            // Translucent black with a faint light ring: visible over a white picture and over black.
            p.setBrush(QColor(0, 0, 0, isDown() ? 200 : hover ? 170 : 140));
            p.setPen(QPen(QColor(255, 255, 255, 48), 1));
            p.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
            p.setPen(Qt::NoPen);
        } else if (checked || hover || isDown()) {
            // A switched-on toggle (Loop) keeps a light chip, so its state never depends on colour.
            const int alpha = checked ? (isDown() ? 70 : hover ? 60 : 46) : (isDown() ? 45 : 26);
            p.setBrush(QColor(255, 255, 255, alpha));
            p.drawRoundedRect(box, 6, 6);
        }

        // White on the control bar's scrim stays readable over bright video (5:1 or more).
        const QColor color = isEnabled() ? QColor(Qt::white) : QColor(0x80, 0x84, 0x8e);
        const qreal  inset = width() * (m_round ? 0.24 : 0.2);
        drawGlyph(p, m_glyph, box.adjusted(inset, inset, -inset, -inset), color);
        if (checked) {
            // ... plus a dot under the glyph, as a second cue next to the chip.
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(width() / 2.0, height() - 5.0), 2.0, 2.0);
        }
    }

  private:
    Glyph m_glyph;
    bool  m_round;
};

// A slider that jumps to the clicked position (instead of paging) and then keeps dragging.
class JumpSlider : public QSlider
{
  public:
    JumpSlider(bool wheel, QWidget* parent)
        : QSlider(Qt::Horizontal, parent)
        , m_wheel(wheel)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover); // the handle grows under the pointer (style sheet)
        setFixedHeight(24);         // the groove stays 4 px; only the area that takes clicks grows
    }

    // Seek bar: text for the value under the pointer (the time), shown while hovering or dragging;
    // hint is added once the pointer rests (the keyboard shortcut).
    void setHoverText(std::function<QString(int value)> text, const QString& hint)
    {
        m_hoverText = std::move(text);
        m_hoverHint = hint;
        setMouseTracking(true); // moves without a button pressed
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        focusWindow(this);
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->pos()))
                setValue(valueAt(event->pos()));
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        QSlider::mouseMoveEvent(event);
        showHoverTip(event->pos(), event->globalPos(), false);
    }

    bool event(QEvent* event) override
    {
        if (m_hoverText && event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            showHoverTip(help->pos(), help->globalPos(), true);
            return true;
        }
        if (m_hoverText && event->type() == QEvent::Leave)
            QToolTip::hideText();
        return QSlider::event(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (m_wheel)
            QSlider::wheelEvent(event);
        else
            event->ignore();
    }

  private:
    int valueAt(const QPoint& pos) const
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const int   x      = pos.x() - groove.x() - handle.width() / 2;
        const int   span   = qMax(1, groove.width() - handle.width());
        return QStyle::sliderValueFromPosition(minimum(), maximum(), x, span, opt.upsideDown);
    }

    void showHoverTip(const QPoint& pos, const QPoint& globalPos, bool withHint)
    {
        if (!m_hoverText || !isEnabled() || maximum() <= minimum())
            return;
        QString text = m_hoverText(isSliderDown() ? value() : valueAt(pos));
        if (withHint && !m_hoverHint.isEmpty())
            text += QLatin1Char('\n') + m_hoverHint;
        // A 1x1 area: the tip follows the pointer instead of staying where it first appeared.
        QToolTip::showText(globalPos, text, this, QRect(pos, QSize(1, 1)));
    }

    std::function<QString(int value)> m_hoverText;
    QString                           m_hoverHint;
    bool                              m_wheel;
};

// Paints video frames / posters letterboxed (scaled up when needed) and the big play button.
class VideoSurface : public QWidget
{
  public:
    enum class Center { None, Play, Replay };

    explicit VideoSurface(QWidget* parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    void setImage(const QImage& image)
    {
        m_image = image;
        update();
    }

    void setAudioTitle(const QString& title)
    {
        m_audioTitle = title;
        update();
    }

    void setCenter(Center center)
    {
        if (center == m_center)
            return;
        m_center = center;
        update();
    }

    void clear()
    {
        m_image = QImage();
        m_audioTitle.clear();
        m_center = Center::None;
        update();
    }

    std::function<void()> onClick;
    std::function<void()> onDoubleClick;
    std::function<void()> onActivity;
    std::function<void(const QPoint&)> onDragOut; // 2.2 drag-out: a drag on the frame (no click then)

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        if (!m_image.isNull()) {
            const QSizeF size = QSizeF(m_image.size()).scaled(QSizeF(this->size()), Qt::KeepAspectRatio);
            const QRectF target(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
            p.drawImage(target, m_image);
        } else if (!m_audioTitle.isEmpty()) {
            const QPointF c(width() / 2.0, height() / 2.0 - 24);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x2b, 0x2d, 0x31));
            p.drawRoundedRect(QRectF(c.x() - 64, c.y() - 64, 128, 128), 24, 24);
            drawGlyph(p, Glyph::Music, QRectF(c.x() - 36, c.y() - 36, 72, 72), QColor(0xb5, 0xba, 0xc1));
            QFont font = this->font();
            font.setPointSizeF(font.pointSizeF() * 1.15);
            p.setFont(font);
            p.setPen(QColor(0xf2, 0xf3, 0xf5));
            const QString text = QFontMetrics(font).elidedText(m_audioTitle, Qt::ElideMiddle, qMax(80, width() - 48));
            p.drawText(QRectF(0, c.y() + 80, width(), 30), Qt::AlignHCenter | Qt::AlignTop, text);
        }

        if (m_center != Center::None) {
            const QPointF c(width() / 2.0, height() / 2.0);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawEllipse(c, kCenterButton, kCenterButton);
            drawGlyph(p, m_center == Center::Replay ? Glyph::Replay : Glyph::Play, QRectF(c.x() - 18, c.y() - 18, 36, 36), Qt::white);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        focusWindow(this);
        if (event->button() == Qt::LeftButton) {
            m_pressPos = event->pos();
            m_pressed  = true;
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const bool pressed = m_pressed;
        m_pressed          = false;
        if (event->button() != Qt::LeftButton || !pressed || !rect().contains(event->pos()))
            return;
        if ((event->pos() - m_pressPos).manhattanLength() <= QApplication::startDragDistance() && onClick)
            onClick();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && onDoubleClick)
            onDoubleClick();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (onActivity)
            onActivity();
        // 2.2 drag-out: the press becomes a drag of the file (the release then plays / pauses nothing).
        if (m_pressed && (event->buttons() & Qt::LeftButton) && onDragOut
            && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_pressed = false;
            onDragOut(m_pressPos);
        }
    }

  private:
    QImage  m_image;
    QString m_audioTitle;
    Center  m_center = Center::None;
    QPoint  m_pressPos;
    bool    m_pressed = false; // the left button went down here and no drag started from it
};

// Download progress ring, spinner, messages and the paused-GIF hint, drawn over the media.
class StatusOverlay : public QWidget
{
  public:
    explicit StatusOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void showProgress(double progress, const QString& text)
    {
        m_mode     = Mode::Progress;
        m_progress = progress;
        m_title    = text;
        m_detail.clear();
        // With Windows' "Show animations" off the indeterminate ring stands still (see paintProgress).
        if (progress < 0 && ui::animationsEnabled()) {
            if (!m_spin.isActive())
                m_spin.start(30, this);
        } else {
            m_spin.stop();
        }
        show();
        update();
    }

    void showMessage(Glyph glyph, const QString& title, const QString& detail, bool error)
    {
        m_spin.stop();
        const bool changed = m_mode != Mode::Message || title != m_title || detail != m_detail;
        m_mode             = Mode::Message;
        m_glyph            = glyph;
        m_title            = title;
        m_detail           = detail;
        m_error            = error;
        show();
        update();
        if (changed) {
            // The text is only painted: give it to screen readers and announce it.
            setAccessibleName(title);
            setAccessibleDescription(detail);
            QAccessibleEvent alert(this, QAccessible::Alert);
            QAccessible::updateAccessibility(&alert);
        }
    }

    void showPaused()
    {
        m_spin.stop();
        m_mode = Mode::Paused;
        show();
        update();
    }

    // A hint in a pill, below (offset from) the centre: e.g. under the big play button of a video
    // that waits for the user to press play.
    void showPrompt(const QString& text, qreal offset)
    {
        m_spin.stop();
        m_mode         = Mode::Prompt;
        m_title        = text;
        m_promptOffset = offset;
        m_detail.clear();
        show();
        update();
    }

    void clear()
    {
        m_spin.stop();
        if (m_mode == Mode::None)
            return;
        m_mode = Mode::None;
        hide();
    }

    bool showsMessage() const { return m_mode == Mode::Message && isVisibleTo(parentWidget()); }

    // Room for buttons at the bottom of the message panel; the owner places its own widget of that
    // size at actionsRect() (this overlay lets every click through, so it can't hold buttons).
    void setActionsSize(const QSize& size)
    {
        if (size == m_actionsSize)
            return;
        m_actionsSize = size;
        update();
    }

    QRect actionsRect() const { return m_mode == Mode::Message ? messageLayout().actions : QRect(); }

  protected:
    void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() != m_spin.timerId()) {
            QWidget::timerEvent(event);
            return;
        }
        m_angle = (m_angle + 9) % 360;
        update(ringRect().toAlignedRect().adjusted(-2, -2, 2, 2)); // never repaint the whole (large) image below
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        switch (m_mode) {
        case Mode::None:
            break;
        case Mode::Progress:
            paintProgress(p);
            break;
        case Mode::Message:
            paintMessage(p);
            break;
        case Mode::Paused: {
            const QPointF c = QRectF(rect()).center();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 140));
            p.drawEllipse(c, 32, 32);
            drawGlyph(p, Glyph::Play, QRectF(c.x() - 17, c.y() - 17, 34, 34), Qt::white);
            break;
        }
        case Mode::Prompt:
            paintPill(p, QRectF(rect()).center().y() + m_promptOffset, m_title);
            break;
        }
    }

  private:
    enum class Mode { None, Progress, Message, Paused, Prompt };

    static constexpr qreal kRing = 24;

    struct MessageLayout {
        QRectF panel; // empty: the window is too small for it
        QRectF glyph;
        QRectF title;
        QRectF detail;
        QRect  actions;
        QFont  titleFont;
    };

    QRectF ringRect() const
    {
        const QPointF c = QRectF(rect()).center();
        return QRectF(c.x() - kRing - 10, c.y() - kRing - 10, 2 * (kRing + 10), 2 * (kRing + 10));
    }

    void paintProgress(QPainter& p)
    {
        const QPointF c = QRectF(rect()).center();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawEllipse(c, kRing + 10, kRing + 10);

        const QRectF ring(c.x() - kRing, c.y() - kRing, 2 * kRing, 2 * kRing);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 55), 4));
        p.drawEllipse(ring);
        p.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap));
        QString title = m_title;
        if (m_progress < 0) {
            p.drawArc(ring, (90 - m_angle) * 16, -100 * 16);
            if (title.isEmpty() && !m_spin.isActive())
                title = i18n::t("Loading…"); // a ring that doesn't move needs words to say it's busy
        } else {
            const double value = qBound(0.0, m_progress, 1.0);
            p.drawArc(ring, 90 * 16, -qMax(1, qRound(value * 360 * 16)));
            QFont font = this->font();
            font.setBold(true);
            font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.85));
            p.setFont(font);
            p.drawText(ring, Qt::AlignCenter, QStringLiteral("%1%").arg(qRound(value * 100)));
        }

        if (!title.isEmpty()) {
            p.setFont(this->font());
            paintPill(p, c.y() + kRing + 20, title);
        }
    }

    // Horizontally centred text pill whose top is at y.
    void paintPill(QPainter& p, qreal y, const QString& title)
    {
        const QFontMetrics fm(font());
        const QString      text = fm.elidedText(title, Qt::ElideRight, qMax(60, width() - 64));
        const int          w    = fm.horizontalAdvance(text) + 24;
        const QRectF       pill(width() / 2.0 - w / 2.0, y, w, fm.height() + 10);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        p.setPen(QColor(0xf2, 0xf3, 0xf5));
        p.drawText(pill, Qt::AlignCenter, text);
    }

    MessageLayout messageLayout() const
    {
        MessageLayout layout;
        int           panelWidth = qMin(440, width() - 48);
        if (!m_actionsSize.isEmpty())
            panelWidth = qMax(panelWidth, qMin(width() - 24, m_actionsSize.width() + 40));
        if (panelWidth < 80)
            return layout;
        const int textWidth = panelWidth - 40;

        layout.titleFont = font();
        layout.titleFont.setBold(true);
        layout.titleFont.setPointSizeF(layout.titleFont.pointSizeF() * 1.1);
        const QFontMetrics titleFm(layout.titleFont);
        const QFontMetrics detailFm(font());
        const QRect        titleRect  = titleFm.boundingRect(QRect(0, 0, textWidth, 1000), Qt::AlignHCenter | Qt::TextWordWrap, m_title);
        const QRect        detailRect = m_detail.isEmpty() ? QRect() : detailFm.boundingRect(QRect(0, 0, textWidth, 1000), Qt::AlignHCenter | Qt::TextWordWrap, m_detail);

        const int glyphSize = 44;
        const int actions   = m_actionsSize.isEmpty() ? 0 : 18 + m_actionsSize.height();
        const int height    = 22 + glyphSize + 14 + titleRect.height() + (m_detail.isEmpty() ? 0 : 6 + detailRect.height()) + actions + 22;
        layout.panel        = QRectF((width() - panelWidth) / 2.0, (this->height() - height) / 2.0, panelWidth, height);

        qreal y      = layout.panel.top() + 22;
        layout.glyph = QRectF(layout.panel.center().x() - glyphSize / 2.0, y, glyphSize, glyphSize);
        y += glyphSize + 14;
        layout.title = QRectF(layout.panel.left() + 20, y, textWidth, titleRect.height());
        y += titleRect.height() + 6;
        layout.detail = QRectF(layout.panel.left() + 20, y, textWidth, detailRect.height());
        if (!m_actionsSize.isEmpty()) {
            layout.actions = QRect(qRound(layout.panel.center().x() - m_actionsSize.width() / 2.0),
                                   qRound(layout.panel.bottom() - 22 - m_actionsSize.height()), m_actionsSize.width(), m_actionsSize.height());
        }
        return layout;
    }

    void paintMessage(QPainter& p)
    {
        const MessageLayout layout = messageLayout();
        if (layout.panel.isEmpty())
            return;

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(17, 18, 20, 215));
        p.drawRoundedRect(layout.panel, 10, 10);

        drawGlyph(p, m_glyph, layout.glyph, m_error ? QColor(0xf2, 0x3f, 0x43) : QColor(0xb5, 0xba, 0xc1));

        p.setFont(layout.titleFont);
        p.setPen(QColor(0xf2, 0xf3, 0xf5));
        p.drawText(layout.title, Qt::AlignHCenter | Qt::TextWordWrap, m_title);

        if (!m_detail.isEmpty()) {
            p.setFont(font());
            p.setPen(QColor(0xb5, 0xba, 0xc1));
            p.drawText(layout.detail, Qt::AlignHCenter | Qt::TextWordWrap, m_detail);
        }
    }

    Mode        m_mode         = Mode::None;
    double      m_progress     = -1.0;
    qreal       m_promptOffset = 0;
    Glyph       m_glyph    = Glyph::Info;
    QString     m_title;
    QString     m_detail;
    bool        m_error = false;
    QSize       m_actionsSize;
    int         m_angle = 0;
    QBasicTimer m_spin;
};

// Translucent black gradient behind controls that float over the media: from the bottom edge up
// (player bar) or from the top edge down (full-screen header). Over a white picture it still gives
// white text and icons 5:1 or more where they are drawn.
class ScrimBar : public QWidget
{
  public:
    ScrimBar(Qt::Edge edge, QWidget* parent)
        : QWidget(parent)
        , m_edge(edge)
    {
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter        p(this);
        QLinearGradient gradient(0, 0, 0, height());
        if (m_edge == Qt::BottomEdge) {
            gradient.setColorAt(0.0, QColor(0, 0, 0, 0));
            gradient.setColorAt(0.35, QColor(0, 0, 0, 140));
            gradient.setColorAt(1.0, QColor(0, 0, 0, 220));
        } else {
            gradient.setColorAt(0.0, QColor(0, 0, 0, 190));
            gradient.setColorAt(0.55, QColor(0, 0, 0, 150));
            gradient.setColorAt(1.0, QColor(0, 0, 0, 0));
        }
        p.fillRect(rect(), gradient);
    }

  private:
    Qt::Edge m_edge;
};

// A short confirmation or hint at the top of the stage ("Image copied", "Volume 45%"), with an
// optional icon. It fades out, unless Windows' "Show animations" is off.
class FlashBubble : public QWidget
{
  public:
    explicit FlashBubble(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        QObject::connect(m_timer, &QTimer::timeout, this, [this] {
            if (ui::animationsEnabled())
                m_fade->start();
            else
                hide();
        });
        m_fade = new QVariantAnimation(this);
        m_fade->setDuration(180);
        m_fade->setStartValue(1.0);
        m_fade->setEndValue(0.0);
        QObject::connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            m_opacity = value.toReal();
            update();
        });
        QObject::connect(m_fade, &QVariantAnimation::finished, this, [this] { hide(); });
    }

    // A repeated flash replaces the shown one and starts its time again.
    void showText(const QString& text, std::optional<Glyph> glyph, int ms, int maxWidth)
    {
        m_fade->stop();
        m_opacity = 1.0;
        m_glyph   = glyph;
        const QFontMetrics fm(font());
        const int          icon = glyph ? 16 + 8 : 0;
        m_text                  = fm.elidedText(text, Qt::ElideRight, qMax(40, maxWidth - 28 - icon));
        resize(14 + icon + fm.horizontalAdvance(m_text) + 14, fm.height() + 14);
        setAccessibleName(text);
        show();
        raise();
        update();
        m_timer->start(ms);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(m_opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(17, 18, 20, 225));
        p.drawRoundedRect(QRectF(rect()), 6, 6);
        qreal x = 14;
        if (m_glyph) {
            // A success is green (6.9:1 on the bubble) and drawn as a check, so it doesn't rely on colour.
            const QColor color = *m_glyph == Glyph::Check ? QColor(0x43, 0xb5, 0x81) : QColor(0xf2, 0xf3, 0xf5);
            drawGlyph(p, *m_glyph, QRectF(x, (height() - 16) / 2.0, 16, 16), color);
            x += 16 + 8;
        }
        p.setPen(QColor(0xf2, 0xf3, 0xf5));
        p.drawText(QRectF(x, 0, width() - x - 14, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }

  private:
    QString              m_text;
    std::optional<Glyph> m_glyph;
    qreal                m_opacity = 1.0;
    QTimer*              m_timer   = nullptr;
    QVariantAnimation*   m_fade    = nullptr;
};

// Every keyboard shortcut of the viewer, over the media (?, F1 or the button in the top bar). A key
// or a click closes it.
class ShortcutSheet : public QWidget
{
  public:
    explicit ShortcutSheet(QWidget* parent)
        : QWidget(parent)
    {
        hide();
        auto row     = [](const char* keys, const char* action) { return Row{i18n::t(keys), i18n::t(action)}; };
        m_columns    = {
            {
                {i18n::t("Browse"),
                 {row("← / →", "Previous / next item"), row("F", "Full screen"), row("Esc", "Leave full screen, then close"),
                  row("? or F1", "Show or hide this list")}},
                {i18n::t("Pictures"),
                 {row("0", "Fit to window"), row("1", "Actual size"), row("+ / −", "Zoom in / out"),
                  row("↑ ↓ Shift+← →", "Move a zoomed picture"), row("Space", "Pause or resume an animation")}},
            },
            {
                {i18n::t("Video and audio"),
                 {row("Space or K", "Play / pause"), row("Shift+← / →", "Back / forward 5 s"), row("Home", "Back to the start"),
                  row("↑ / ↓", "Volume"), row("M", "Mute"), row("L", "Loop")}},
                {i18n::t("File"),
                 {row("Ctrl+C", "Copy the picture or frame"), row("Ctrl+Shift+C", "Copy the file"), row("Ctrl+S", "Save as…"),
                  row("Ctrl+O", "Open with default app"), row("Drag picture or name", "Copy the file to a folder or app"),
                  row("Enter", "Press the highlighted button")}},
            },
        };
        QStringList lines;
        for (const QVector<Group>& column : qAsConst(m_columns)) {
            for (const Group& group : column) {
                for (const Row& r : group.rows)
                    lines << r.keys + QLatin1String(": ") + r.action;
            }
        }
        setAccessibleName(i18n::t("Keyboard shortcuts"));
        setAccessibleDescription(lines.join(QLatin1Char('\n')));
    }

  protected:
    void mousePressEvent(QMouseEvent*) override
    {
        hide();
        focusWindow(this);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0, 0, 0, 110));

        QFont bold = font();
        bold.setBold(true);
        QFont titleFont = bold;
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
        const QFontMetrics fm(font());
        const QFontMetrics boldFm(bold);
        const QFontMetrics titleFm(titleFont);
        const QString      title = i18n::t("Keyboard shortcuts");
        const QString      hint  = i18n::t("Press Esc or click anywhere to close.");

        int keyWidth    = 0;
        int actionWidth = 0;
        int groupWidth  = 0;
        for (const QVector<Group>& column : qAsConst(m_columns)) {
            for (const Group& group : column) {
                groupWidth = qMax(groupWidth, boldFm.horizontalAdvance(group.title));
                for (const Row& r : group.rows) {
                    keyWidth    = qMax(keyWidth, boldFm.horizontalAdvance(r.keys));
                    actionWidth = qMax(actionWidth, fm.horizontalAdvance(r.action));
                }
            }
        }
        const int                     pad         = 24;
        const int                     gap         = 40; // between the columns
        const int                     columnWidth = qMax(groupWidth, keyWidth + 16 + actionWidth);
        const QVector<QVector<Group>> oneColumn{m_columns.at(0) + m_columns.at(1)};

        // The roomiest layout that fits: two columns or one, then tighter rows without the closing
        // hint. In a very small window the best of them is drawn smaller.
        struct Layout {
            const QVector<QVector<Group>>* columns   = nullptr;
            int                            rowHeight = 0;
            int                            groupGap  = 0;
            bool                           withHint  = false;
            int                            width     = 0;
            int                            height    = 0;
            qreal                          scale     = 0;
        };
        auto measure = [&](const QVector<QVector<Group>>& columns, bool compact) {
            Layout l;
            l.columns         = &columns;
            l.rowHeight       = fm.height() + (compact ? 2 : 6);
            l.groupGap        = compact ? 8 : 14;
            l.withHint        = !compact;
            int contentHeight = 0;
            for (const QVector<Group>& column : columns) {
                int h = 0;
                for (const Group& group : column)
                    h += boldFm.height() + 6 + group.rows.size() * l.rowHeight + l.groupGap;
                contentHeight = qMax(contentHeight, h - l.groupGap);
            }
            const int contentWidth = columns.size() * columnWidth + (columns.size() - 1) * gap;
            l.width  = qMax(contentWidth, qMax(titleFm.horizontalAdvance(title), l.withHint ? fm.horizontalAdvance(hint) : 0)) + 2 * pad;
            l.height = pad + titleFm.height() + 16 + contentHeight + (l.withHint ? 18 + fm.height() : 0) + pad;
            l.scale  = qMin(1.0, qMin((width() - 32.0) / l.width, (height() - 16.0) / l.height));
            return l;
        };
        Layout layout;
        for (const Layout& candidate : {measure(m_columns, false), measure(oneColumn, false), measure(m_columns, true), measure(oneColumn, true)}) {
            if (candidate.scale > layout.scale + 0.001)
                layout = candidate;
            if (layout.scale >= 1.0)
                break;
        }
        const QVector<QVector<Group>>& columns   = *layout.columns;
        const int                      rowHeight = layout.rowHeight;
        const int                      groupGap  = layout.groupGap;

        p.translate(width() / 2.0, height() / 2.0);
        p.scale(layout.scale, layout.scale);
        const QRectF panel(-layout.width / 2.0, -layout.height / 2.0, layout.width, layout.height);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(17, 18, 20, 240));
        p.drawRoundedRect(panel, 10, 10);

        const qreal left = panel.left() + pad;
        qreal       y    = panel.top() + pad;
        p.setFont(titleFont);
        p.setPen(QColor(0xf2, 0xf3, 0xf5));
        p.drawText(QRectF(left, y, panel.width() - 2 * pad, titleFm.height()), Qt::AlignLeft | Qt::AlignVCenter, title);
        y += titleFm.height() + 16;

        for (int c = 0; c < columns.size(); ++c) {
            const qreal x  = left + c * (columnWidth + gap);
            qreal       cy = y;
            for (const Group& group : columns.at(c)) {
                p.setFont(bold);
                p.setPen(QColor(0x94, 0x9c, 0xf7)); // 6.9:1 on the panel
                p.drawText(QRectF(x, cy, columnWidth, boldFm.height()), Qt::AlignLeft | Qt::AlignVCenter, group.title);
                cy += boldFm.height() + 6;
                for (const Row& r : group.rows) {
                    p.setFont(bold);
                    p.setPen(QColor(0xf2, 0xf3, 0xf5));
                    p.drawText(QRectF(x, cy, keyWidth, rowHeight), Qt::AlignLeft | Qt::AlignVCenter, r.keys);
                    p.setFont(font());
                    p.setPen(QColor(0xb5, 0xba, 0xc1)); // 8.4:1
                    p.drawText(QRectF(x + keyWidth + 16, cy, actionWidth, rowHeight), Qt::AlignLeft | Qt::AlignVCenter, r.action);
                    cy += rowHeight;
                }
                cy += groupGap;
            }
        }

        if (layout.withHint) {
            p.setFont(font());
            p.setPen(QColor(0xb5, 0xba, 0xc1));
            p.drawText(QRectF(left, panel.bottom() - pad - fm.height(), panel.width() - 2 * pad, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, hint);
        }
    }

  private:
    struct Row {
        QString keys;
        QString action;
    };
    struct Group {
        QString      title;
        QVector<Row> rows;
    };
    QVector<QVector<Group>> m_columns;
};

struct Decoded {
    QImage image;
    QSize  size;
    bool   tooLarge = false;
};

Decoded decodeImage(const QString& path)
{
    Decoded      result;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    QSize size = reader.size();
    if (size.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90))
        size.transpose();
    result.size = size;
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaxPixels) {
        result.tooLarge = true;
        return result;
    }
    QImage image = reader.read();
    if (!image.isNull() && image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    result.image = image;
    if (!image.isNull())
        result.size = image.size();
    return result;
}

QPointer<MediaViewer> g_viewer; // the open viewer is reused for the next item

} // namespace

// ============================================================================================
// ImageCanvas
// ============================================================================================

ImageCanvas::ImageCanvas(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(200, 150);
    m_rescale = new QTimer(this);
    m_rescale->setSingleShot(true);
    m_rescale->setInterval(kRescaleDelayMs);
    connect(m_rescale, &QTimer::timeout, this, [this] { rescale(); });
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(1);
}

ImageCanvas::~ImageCanvas()
{
    // A worker posts its result to this object: it must be done before the object goes.
    m_pool->clear();
    m_pool->waitForDone();
}

void ImageCanvas::setImage(const QImage& image, bool resetView)
{
    const QSize oldSize = m_image.size();
    m_image             = image;
    m_display           = QImage(); // made from the previous picture
    if (resetView || oldSize.isEmpty() || image.isNull()) {
        setFit();
        return;
    }
    if (m_fit) {
        m_zoom = fitZoom();
        emit zoomChanged(m_zoom);
    } else if (oldSize.width() != image.width() && image.width() > 0) {
        // Same picture at a different resolution (placeholder -> full image): keep its size on screen.
        m_zoom = qBound(kMinZoom, m_zoom * oldSize.width() / image.width(), kMaxZoom);
        emit zoomChanged(m_zoom);
    }
    clampOffset();
    update();
    scheduleRescale(); // an animation restarts it with every frame: only a paused one gets the copy
}

qreal ImageCanvas::scale() const
{
    return m_zoom / devicePixelRatioF();
}

qreal ImageCanvas::fitZoom() const
{
    if (m_image.isNull())
        return 1.0;
    const qreal dpr = devicePixelRatioF();
    const qreal zx  = (width() - 24) * dpr / m_image.width();
    const qreal zy  = (height() - 24) * dpr / m_image.height();
    return qMax(0.01, qMin(1.0, qMin(zx, zy))); // never upscale when fitting, not even by the display scaling
}

void ImageCanvas::setFit()
{
    m_fit    = true;
    m_zoom   = fitZoom();
    m_offset = {};
    applyZoom();
}

void ImageCanvas::setActualSize()
{
    zoomTo(1.0, QPointF(width() / 2.0, height() / 2.0));
}

void ImageCanvas::zoomTo(qreal zoom, const QPointF& anchor)
{
    if (m_image.isNull())
        return;
    const qreal newZoom = qBound(qMin(kMinZoom, fitZoom()), zoom, kMaxZoom);
    // The image point under the anchor stays there.
    const QPointF at = anchor - QPointF(width() / 2.0, height() / 2.0);
    m_offset         = at - (at - m_offset) * (newZoom / m_zoom);
    m_zoom           = newZoom;
    m_fit            = false;
    applyZoom();
}

void ImageCanvas::zoomBy(qreal factor)
{
    zoomTo(m_zoom * factor, QPointF(width() / 2.0, height() / 2.0));
}

void ImageCanvas::panBy(const QPointF& delta)
{
    m_offset += delta;
    clampOffset();
    update();
}

qreal ImageCanvas::zoom() const
{
    return m_zoom;
}

bool ImageCanvas::isFit() const
{
    return m_fit;
}

bool ImageCanvas::canPan() const
{
    if (m_image.isNull())
        return false;
    const QSizeF scaled = QSizeF(m_image.size()) * scale();
    return scaled.width() > width() + 0.5 || scaled.height() > height() + 0.5;
}

void ImageCanvas::setCursorHidden(bool hidden)
{
    if (hidden == m_cursorHidden)
        return;
    m_cursorHidden = hidden;
    updateCursor();
}

void ImageCanvas::applyZoom()
{
    clampOffset();
    update();
    emit zoomChanged(m_zoom);
    scheduleRescale();
}

void ImageCanvas::clampOffset()
{
    const QSizeF scaled = QSizeF(m_image.size()) * scale();
    const qreal  maxX   = qMax(0.0, (scaled.width() - width()) / 2.0);
    const qreal  maxY   = qMax(0.0, (scaled.height() - height()) / 2.0);
    m_offset.setX(qBound(-maxX, m_offset.x(), maxX));
    m_offset.setY(qBound(-maxY, m_offset.y(), maxY));
    updateCursor();
}

void ImageCanvas::updateCursor()
{
    if (!m_dragging)
        setCursor(m_cursorHidden ? Qt::BlankCursor : canPan() ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

// Bilinear sampling (what QPainter does) reads only a few source pixels per screen pixel, so a strong
// downscale turns text into broken strokes and fine detail into moiré. Once the zoom has settled, a
// copy resampled with area averaging is made at exactly the drawn size and used instead.
void ImageCanvas::scheduleRescale()
{
    if (!m_display.isNull() && m_displayZoom == m_zoom)
        return; // still matches
    m_display = QImage();
    ++m_displayJob;
    if (m_image.isNull() || m_zoom >= kSmoothBelow) {
        m_rescale->stop();
        return;
    }
    m_rescale->start();
}

void ImageCanvas::rescale()
{
    if (m_image.isNull() || m_zoom >= kSmoothBelow)
        return;
    const qreal   dpr    = devicePixelRatioF();
    const qreal   zoom   = m_zoom;
    const quint64 job    = m_displayJob;
    const QSize   target = (QSizeF(m_image.size()) * zoom).toSize().expandedTo(QSize(1, 1));
    if (static_cast<qint64>(m_image.width()) * m_image.height() <= kRescaleInlinePixels) {
        setDisplay(job, zoom, dpr, m_image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        return;
    }
    // Very large pictures take tens of milliseconds: never on the GUI thread.
    const QImage source = m_image;
    m_pool->clear();
    m_pool->start([this, source, target, job, zoom, dpr] {
        const QImage scaled = source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QMetaObject::invokeMethod(this, [this, job, zoom, dpr, scaled] { setDisplay(job, zoom, dpr, scaled); }, Qt::QueuedConnection);
    });
}

void ImageCanvas::setDisplay(quint64 job, qreal zoom, qreal dpr, QImage image)
{
    if (job != m_displayJob || image.isNull())
        return;
    image.setDevicePixelRatio(dpr);
    m_display     = image;
    m_displayZoom = zoom;
    m_displayDpr  = dpr;
    update();
}

void ImageCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1e, 0x1f, 0x22));
    if (m_image.isNull())
        return;
    const qreal  dpr  = devicePixelRatioF();
    const QSizeF size = QSizeF(m_image.size()) * scale();
    QPointF      topLeft = QPointF(width() / 2.0, height() / 2.0) + m_offset - QPointF(size.width() / 2.0, size.height() / 2.0);
    // On whole device pixels, so that 100% puts every image pixel on exactly one screen pixel.
    topLeft = QPointF(std::round(topLeft.x() * dpr) / dpr, std::round(topLeft.y() * dpr) / dpr);
    if (!m_display.isNull() && m_displayZoom == m_zoom && qFuzzyCompare(m_displayDpr, dpr)) {
        p.drawImage(topLeft, m_display); // already at the drawn size (device pixels)
        return;
    }
    // Exactly 100%: pixel for pixel. Up to 400% smoothed; beyond that sharp pixels, for inspecting them.
    p.setRenderHint(QPainter::SmoothPixmapTransform, qAbs(m_zoom - 1.0) > 1e-6 && m_zoom < 4.0);
    p.drawImage(QRectF(topLeft, size), m_image);
}

void ImageCanvas::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (m_image.isNull() || delta == 0) {
        event->ignore();
        return;
    }
    // Proportional to the wheel delta so touchpads zoom smoothly; one notch = ~20%. Around the cursor.
    zoomTo(m_zoom * std::pow(1.0015, delta), event->position());
}

void ImageCanvas::mousePressEvent(QMouseEvent* event)
{
    focusWindow(this);
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging   = true;
    m_moved      = false;
    m_dragStart  = event->pos();
    m_dragOffset = m_offset;
    if (canPan())
        setCursor(Qt::ClosedHandCursor);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_activityClock.isValid() || m_activityClock.elapsed() >= 50) {
        m_activityClock.start();
        emit activity();
    }
    if (!m_dragging)
        return;
    if (!m_moved && (event->pos() - m_dragStart).manhattanLength() < QApplication::startDragDistance())
        return;
    if (!m_moved && !canPan()) {
        // 2.2 drag-out: nothing to pan, so the drag takes the file out of the viewer (no click).
        m_dragging = false;
        updateCursor();
        emit dragOutRequested(m_dragStart);
        return;
    }
    m_moved  = true;
    m_offset = m_dragOffset + QPointF(event->pos() - m_dragStart);
    clampOffset();
    update();
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    clampOffset(); // restores the cursor
    if (!m_moved)
        emit clicked();
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_image.isNull())
        return;
    if (!m_fit) {
        setFit();
        return;
    }
    // Into the clicked spot: 100%, or 200% when the picture already fits at 100% (no change otherwise).
    zoomTo(fitZoom() >= 1.0 ? 2.0 : 1.0, event->localPos());
}

void ImageCanvas::resizeEvent(QResizeEvent*)
{
    if (m_fit) {
        const qreal zoom = fitZoom();
        if (zoom != m_zoom) {
            m_zoom = zoom;
            emit zoomChanged(m_zoom);
            scheduleRescale();
        }
    }
    clampOffset();
}

// ============================================================================================
// MediaViewer
// ============================================================================================

struct MediaViewer::Private : public QObject {
    enum class Content { None, Image, Animation, Video, Audio, File };

    // Buttons inside the message panel: the way out of an error, next to its explanation.
    struct PanelButtons {
        bool retry = false;
        bool open  = false;
        bool store = false;
    };

    Private(MediaViewer* viewer, Core* c);
    ~Private() override;

    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildUi();
    void setGallery(const QStringList& newKeys, int newIndex);
    void showItem(int i);
    void step(int delta);
    void teardown();

    const MediaEntry* entry() const;
    QString           currentKey() const;
    QSize             stillTarget() const;
    bool              isPlayable() const { return content == Content::Video || content == Content::Audio; }
    bool              isPicture() const { return content == Content::Image || content == Content::Animation; }
    bool              fetchesAutomatically() const;
    bool              waitingForPlay() const;
    bool              canOpen() const;
    QString           notReadyText() const;

    void applyStill(bool reset);
    void startLoading();
    void loadImage(const QString& path);
    void onDecoded(quint64 forGeneration, const Decoded& result);
    void startAnimation(const QString& path);
    void openPlayer(const QString& path);
    void onPlayerLoaded();
    void onEntryChanged(const QString& changedKey);
    void primaryAction();

    void         updateTopBar();
    void         elideName();
    void         updateOverlay();
    PanelButtons showStatus();
    void         setPanelButtons(const PanelButtons& buttons);
    void         placeMessageActions();
    void         updateButtons();
    void         updateZoomButtons();
    void         updatePlaybackUi();
    void         updateTime(qint64 positionMs);
    void         layoutStage();
    void         placeFlash();
    void         updateFrameSize();
    void         showControls();
    void         autoHideControls();
    void         fadeOut(const QList<QWidget*>& widgets);
    void         stopFade();
    void         onWindowStateChanged();
    void         setShown(QWidget* widget, bool shown);

    void togglePlay();
    void toggleGifPause();
    void seekTo(qint64 ms);
    void seekBy(qint64 deltaMs);
    void setVolume(int percent, bool persist);
    void setMuted(bool on, bool notify);
    void toggleMute();
    void toggleFullscreen();
    void toggleShortcuts();
    void copyCurrent();
    void saveCurrent();
    void openCurrent();
    // 2.2 drag-out: drag the shown file out (pressFraction: where in the media the press was, 0..1),
    // and Ctrl+Shift+C, its keyboard alternative.
    void startDragOut(const QPointF& pressFraction);
    void copyFileCurrent();
    void openStore();
    void activateDefault();
    void flash(const QString& text, std::optional<Glyph> glyph, int ms);
    void flashVolume(bool muteKey);
    bool handleKey(QKeyEvent* event);

    MediaViewer*   q;
    QPointer<Core> core;
    QStringList    keys;
    int            index = -1;
    QString        inUseKey;
    QSet<QString>  requested; // asked for by the user (opened on, Play / Download pressed): fetched whatever the size

    // State of the shown item (reset by teardown()).
    Content            content     = Content::None;
    bool               started     = false; // loading of the downloaded file has begun
    bool               loaded      = false; // full picture / first animation frame shown
    MediaStill::Source stillSource = MediaStill::None;
    QImage             fullImage;
    QString            errorTitle;
    QString            errorDetail;
    QString            storeApp; // Microsoft Store extension the playback error names
    bool               tooLarge = false;
    QSize              tooLargeSize;
    quint64            generation = 0;
    QMovie*            movie      = nullptr;
    bool               gifAutoPaused = false; // the animation was paused because the window got minimized
    mf::VideoPlayer*   player     = nullptr;
    bool               wasPlaying       = false; // last known player->isPlaying() (playbackStarted edge)
    bool               autoplay         = true;  // play once loaded; cleared when an inline video started meanwhile
    bool               playerFailed     = false;
    bool               hasFrame         = false;
    bool               resumeAfterScrub = false;
    qint64             pendingSeekMs    = -1;
    QElapsedTimer      pendingSeekClock;
    QElapsedTimer      scrubClock;
    QSize              frameSize;

    // Playback preferences, kept while the viewer is open.
    int  volume       = 80;
    bool muted        = false;
    bool loop         = false;
    bool wasMaximized = false;

    QThreadPool        pool; // image decoding; waited for in the destructor
    QTimer*            downloadTimer = nullptr;
    QTimer*            hideTimer     = nullptr;
    QVariantAnimation* fade          = nullptr; // auto-hidden controls fading out
    QList<QPointer<QWidget>> fading;              // the widgets that fade (each with its own opacity effect meanwhile)

    QString        displayName; // displayNameFor() of the shown item (nameLabel shows it elided)
    QPoint         namePressPos;        // 2.2 drag-out: the left button went down on the name here...
    bool           namePressed = false; // ... and no drag started from it yet
    QWidget*       topBar       = nullptr;
    QLabel*        nameLabel    = nullptr;
    QLabel*        metaLabel    = nullptr;
    GlyphButton*   helpButton   = nullptr;
    QLabel*        counterLabel = nullptr;
    QWidget*       stage        = nullptr;
    ImageCanvas*   canvas       = nullptr;
    VideoSurface*  surface      = nullptr;
    StatusOverlay* overlay      = nullptr;
    GlyphButton*   prevButton   = nullptr;
    GlyphButton*   nextButton   = nullptr;
    FlashBubble*   flashBubble  = nullptr;
    ShortcutSheet* sheet        = nullptr;

    QWidget*     messageActions = nullptr;
    QBoxLayout*  messageLayout  = nullptr;
    QPushButton* messageStore   = nullptr;
    QPushButton* messageOpen    = nullptr;
    QPushButton* messageRetry   = nullptr;

    // Full screen: name, position and actions float over the media (the bars are hidden).
    ScrimBar*    fsHeader   = nullptr;
    QLabel*      fsName     = nullptr;
    QLabel*      fsCounter  = nullptr;
    GlyphButton* fsCopy     = nullptr;
    GlyphButton* fsSave     = nullptr;
    GlyphButton* fsOpen     = nullptr;
    GlyphButton* exitButton = nullptr;

    ScrimBar*    controls         = nullptr;
    GlyphButton* playButton       = nullptr;
    QLabel*      timeLabel        = nullptr;
    JumpSlider*  seekSlider       = nullptr;
    GlyphButton* muteButton       = nullptr;
    JumpSlider*  volumeSlider     = nullptr;
    GlyphButton* loopButton       = nullptr;
    GlyphButton* fullscreenButton = nullptr;

    QWidget*     actionBar    = nullptr;
    QPushButton* fitButton    = nullptr;
    QPushButton* actualButton = nullptr;
    QLabel*      zoomLabel    = nullptr;
    QPushButton* actionButton = nullptr;
    QPushButton* copyButton   = nullptr;
    QPushButton* saveButton   = nullptr;
    QPushButton* folderButton = nullptr;
    QPushButton* openButton   = nullptr;
};

MediaViewer::Private::Private(MediaViewer* viewer, Core* c)
    : q(viewer)
    , core(c)
{
    pool.setMaxThreadCount(1);
    const Settings& s = Settings::instance();
    volume            = qBound(0, s.videoVolume, 100);
    muted             = s.videosStartMuted;
    loop              = s.loopVideos;

    downloadTimer = new QTimer(this);
    downloadTimer->setSingleShot(true);
    downloadTimer->setInterval(kDownloadDelay);
    connect(downloadTimer, &QTimer::timeout, this, [this] {
        const MediaEntry* e = entry();
        if (core && e && e->state == MediaState::Idle && fetchesAutomatically())
            core->download(currentKey(), false);
    });

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    hideTimer->setInterval(kHideControlsMs);
    connect(hideTimer, &QTimer::timeout, this, [this] { autoHideControls(); });

    // The opacity effect renders a widget (and the video behind it) offscreen: it only exists while
    // the fade runs.
    fade = new QVariantAnimation(this);
    fade->setDuration(kFadeOutMs);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        for (const QPointer<QWidget>& w : qAsConst(fading)) {
            if (auto* effect = w ? qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect()) : nullptr)
                effect->setOpacity(value.toReal());
        }
    });
    connect(fade, &QVariantAnimation::finished, this, [this] {
        const QList<QPointer<QWidget>> faded = fading;
        fading.clear();
        for (const QPointer<QWidget>& w : faded) {
            if (w) {
                w->hide();
                w->setGraphicsEffect(nullptr);
            }
        }
    });
}

MediaViewer::Private::~Private()
{
    pool.clear();
    pool.waitForDone();
}

bool MediaViewer::Private::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::Resize:
        if (watched == stage)
            layoutStage();
        else if (watched == nameLabel || watched == fsName)
            elideName();
        break;
    case QEvent::WindowStateChange:
        if (watched == q)
            onWindowStateChanged();
        break;
    case QEvent::MouseButtonPress:
        if (qobject_cast<QPushButton*>(watched))
            focusWindow(static_cast<QWidget*>(watched));
        if ((watched == nameLabel || watched == fsName) && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            // 2.2 drag-out: the name can always be dragged, also while a zoomed picture pans.
            namePressPos = static_cast<QMouseEvent*>(event)->pos();
            namePressed  = true;
            focusWindow(static_cast<QWidget*>(watched));
            return true; // the label gets the moves that follow
        }
        break;
    case QEvent::MouseMove:
        if ((watched == nameLabel || watched == fsName) && namePressed) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (!(me->buttons() & Qt::LeftButton)) {
                namePressed = false;
            } else if ((me->pos() - namePressPos).manhattanLength() >= QApplication::startDragDistance()) {
                namePressed = false;
                startDragOut(QPointF(0.5, 0.5));
                return true;
            }
        }
        break;
    case QEvent::MouseButtonRelease:
        if (watched == nameLabel || watched == fsName)
            namePressed = false;
        break;
    case QEvent::KeyPress: {
        // A focused button keeps Tab, Space and Enter; every other key is a viewer shortcut (the
        // arrow keys would move the focus between the buttons instead of through the gallery).
        if (!qobject_cast<QPushButton*>(watched))
            break;
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            break;
        default:
            if (handleKey(key))
                return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void MediaViewer::Private::buildUi()
{
    auto textButton = [this](const QString& text, QWidget* parent) {
        auto* button = new QPushButton(text, parent);
        // Tab reaches it, a click doesn't focus it: Space and the arrows keep acting on the media.
        button->setFocusPolicy(Qt::TabFocus);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->installEventFilter(this);
        return button;
    };
    auto nameLabelIn = [](QWidget* parent, const char* objectName) {
        auto* label = new QLabel(parent);
        label->setObjectName(QString::fromLatin1(objectName));
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        // File names come from chat links (anyone can post one): never let a name be read as rich text.
        label->setTextFormat(Qt::PlainText);
        // Left-aligned like the rest of the window, also when a name is in a right-to-left script.
        label->setAlignment(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
        return label;
    };

    // ---- top bar: name, details, gallery position --------------------------------------------
    topBar = new QWidget(q);
    topBar->setObjectName(QString::fromLatin1("tsmediaTopBar"));
    nameLabel = nameLabelIn(topBar, "tsmediaTitle");
    nameLabel->installEventFilter(this);
    metaLabel = nameLabelIn(topBar, "tsmediaMeta");
    helpButton = new GlyphButton(Glyph::Help, 28, false, topBar);
    setIconAction(helpButton, i18n::t("Keyboard shortcuts"), i18n::t("Keyboard shortcuts (?)"));
    counterLabel = new QLabel(topBar);
    counterLabel->setObjectName(QString::fromLatin1("tsmediaCounter"));
    counterLabel->setTextFormat(Qt::PlainText);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(nameLabel);
    titles->addWidget(metaLabel);
    auto* top = new QHBoxLayout(topBar);
    top->setContentsMargins(16, 10, 16, 10);
    top->setSpacing(12);
    top->addLayout(titles, 1);
    top->addWidget(helpButton, 0, Qt::AlignVCenter);
    top->addWidget(counterLabel, 0, Qt::AlignVCenter);

    // ---- stage: picture / video, overlays -----------------------------------------------------
    stage = new QWidget(q);
    stage->setObjectName(QString::fromLatin1("tsmediaStage"));
    stage->setMinimumSize(320, 220);
    stage->installEventFilter(this);

    canvas  = new ImageCanvas(stage);
    surface = new VideoSurface(stage);
    surface->hide();
    overlay = new StatusOverlay(stage);

    messageActions = new QWidget(stage);
    messageLayout  = new QBoxLayout(QBoxLayout::LeftToRight, messageActions);
    messageLayout->setContentsMargins(0, 0, 0, 0);
    messageLayout->setSpacing(8);
    messageStore = textButton(i18n::t("Get it from Microsoft Store"), messageActions);
    messageOpen  = textButton(i18n::t("Open with default app"), messageActions);
    messageOpen->setToolTip(i18n::t("Open with the app Windows uses for this file type (Ctrl+O)"));
    messageRetry = textButton(i18n::t("Retry"), messageActions);
    messageRetry->setToolTip(i18n::t("Download it again (Enter)"));
    messageLayout->addWidget(messageStore);
    messageLayout->addWidget(messageOpen);
    messageLayout->addWidget(messageRetry);
    messageActions->hide();

    controls = new ScrimBar(Qt::BottomEdge, stage);
    controls->hide();
    playButton = new GlyphButton(Glyph::Play, 34, false, controls);
    timeLabel  = new QLabel(controls);
    timeLabel->setObjectName(QString::fromLatin1("tsmediaTime"));
    seekSlider = new JumpSlider(false, controls);
    seekSlider->setRange(0, 0);
    seekSlider->setAccessibleName(i18n::t("Seek"));
    seekSlider->setHoverText([](int value) { return formatDuration(value); }, i18n::t("Shift+← / Shift+→ skips 5 s"));
    muteButton   = new GlyphButton(Glyph::VolumeHigh, 34, false, controls);
    volumeSlider = new JumpSlider(true, controls);
    volumeSlider->setRange(0, 100);
    volumeSlider->setSingleStep(5);
    volumeSlider->setPageStep(10);
    volumeSlider->setFixedWidth(96); // 5% steps of about 4 px
    volumeSlider->setValue(volume);
    volumeSlider->setAccessibleName(i18n::t("Volume"));
    volumeSlider->setToolTip(i18n::t("Volume (↑ / ↓)"));
    loopButton = new GlyphButton(Glyph::Loop, 34, false, controls);
    loopButton->setCheckable(true);
    loopButton->setChecked(loop);
    setIconAction(loopButton, i18n::t("Loop"), loop ? i18n::t("Loop: on (L)") : i18n::t("Loop: off (L)"));
    fullscreenButton = new GlyphButton(Glyph::FullscreenEnter, 34, false, controls);
    setIconAction(fullscreenButton, i18n::t("Full screen"), i18n::t("Full screen (F)"));

    auto* bar = new QHBoxLayout(controls);
    bar->setContentsMargins(10, 26, 10, 8);
    bar->setSpacing(6);
    bar->addWidget(playButton);
    bar->addWidget(timeLabel);
    bar->addSpacing(4);
    bar->addWidget(seekSlider, 1);
    bar->addSpacing(4);
    bar->addWidget(muteButton);
    bar->addWidget(volumeSlider);
    bar->addWidget(loopButton);
    bar->addWidget(fullscreenButton);

    prevButton = new GlyphButton(Glyph::ChevronLeft, 44, true, stage);
    setIconAction(prevButton, i18n::t("Previous item"), i18n::t("Previous (←)"));
    nextButton = new GlyphButton(Glyph::ChevronRight, 44, true, stage);
    setIconAction(nextButton, i18n::t("Next item"), i18n::t("Next (→)"));

    // ---- full-screen header -------------------------------------------------------------------
    fsHeader = new ScrimBar(Qt::TopEdge, stage);
    fsHeader->hide();
    fsName = nameLabelIn(fsHeader, "tsmediaFsTitle");
    fsName->installEventFilter(this);
    fsCounter = new QLabel(fsHeader);
    fsCounter->setObjectName(QString::fromLatin1("tsmediaFsCounter"));
    fsCounter->setTextFormat(Qt::PlainText);
    fsCopy = new GlyphButton(Glyph::Copy, 34, false, fsHeader);
    fsSave = new GlyphButton(Glyph::Save, 34, false, fsHeader);
    setIconAction(fsSave, i18n::t("Save as…"), i18n::t("Save as… (Ctrl+S)"));
    fsOpen = new GlyphButton(Glyph::Open, 34, false, fsHeader);
    setIconAction(fsOpen, i18n::t("Open with default app"), i18n::t("Open with default app (Ctrl+O)"));
    exitButton = new GlyphButton(Glyph::FullscreenExit, 40, true, fsHeader);
    setIconAction(exitButton, i18n::t("Exit full screen"), i18n::t("Exit full screen (Esc)"));

    auto* header = new QHBoxLayout(fsHeader);
    header->setContentsMargins(20, 12, 16, 28); // the bottom part is where the gradient fades out
    header->setSpacing(6);
    header->addWidget(fsName, 1);
    header->addSpacing(6);
    header->addWidget(fsCounter, 0, Qt::AlignVCenter);
    header->addSpacing(10);
    header->addWidget(fsCopy, 0, Qt::AlignVCenter);
    header->addWidget(fsSave, 0, Qt::AlignVCenter);
    header->addWidget(fsOpen, 0, Qt::AlignVCenter);
    header->addSpacing(6);
    header->addWidget(exitButton, 0, Qt::AlignVCenter);

    flashBubble = new FlashBubble(stage);
    sheet       = new ShortcutSheet(stage);

    // ---- action bar ---------------------------------------------------------------------------
    actionBar = new QWidget(q);
    actionBar->setObjectName(QString::fromLatin1("tsmediaActionBar"));
    fitButton = textButton(i18n::t("Fit"), actionBar);
    fitButton->setCheckable(true); // shows which of the two views is active
    fitButton->setToolTip(i18n::t("Fit to window (0)"));
    actualButton = textButton(i18n::t("100%"), actionBar);
    actualButton->setCheckable(true);
    zoomLabel = new QLabel(actionBar);
    zoomLabel->setMinimumWidth(48);
    actionButton = textButton(QString(), actionBar);
    setPrimary(actionButton, true);
    actionButton->hide();
    copyButton = textButton(i18n::t("Copy image"), actionBar);
    copyButton->setToolTip(i18n::t("Copy to clipboard (Ctrl+C)"));
    saveButton = textButton(i18n::t("Save as…"), actionBar);
    saveButton->setToolTip(i18n::t("Save a copy (Ctrl+S)"));
    folderButton = textButton(i18n::t("Show in folder"), actionBar);
    folderButton->setToolTip(i18n::t("Show the file in Explorer"));
    openButton = textButton(i18n::t("Open with default app"), actionBar);
    openButton->setToolTip(i18n::t("Open with the app Windows uses for this file type (Ctrl+O)"));

    auto* actions = new QHBoxLayout(actionBar);
    actions->setContentsMargins(12, 8, 12, 10);
    actions->setSpacing(6);
    actions->addWidget(fitButton);
    actions->addWidget(actualButton);
    actions->addWidget(zoomLabel);
    actions->addStretch(1);
    actions->addWidget(actionButton);
    actions->addWidget(copyButton);
    actions->addWidget(saveButton);
    actions->addWidget(folderButton);
    actions->addWidget(openButton);

    auto* layout = new QVBoxLayout(q);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);
    layout->addWidget(stage, 1);
    layout->addWidget(actionBar);

    // ---- behaviour ----------------------------------------------------------------------------
    connect(canvas, &ImageCanvas::zoomChanged, this, [this](qreal z) {
        zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
        updateZoomButtons();
    });
    connect(canvas, &ImageCanvas::clicked, this, [this] {
        if (content == Content::Animation && movie)
            toggleGifPause();
    });
    connect(canvas, &ImageCanvas::activity, this, [this] {
        if (q->isFullScreen())
            showControls(); // pictures auto-hide their controls in full screen only
    });
    // 2.2 drag-out: from the picture when it fits (zoomed in, a drag pans), and from the video frame.
    connect(canvas, &ImageCanvas::dragOutRequested, this, [this](const QPoint& pos) {
        startDragOut(QPointF(canvas->width() > 0 ? static_cast<qreal>(pos.x()) / canvas->width() : 0.5,
                             canvas->height() > 0 ? static_cast<qreal>(pos.y()) / canvas->height() : 0.5));
    });
    surface->onDragOut = [this](const QPoint& pos) {
        startDragOut(QPointF(surface->width() > 0 ? static_cast<qreal>(pos.x()) / surface->width() : 0.5,
                             surface->height() > 0 ? static_cast<qreal>(pos.y()) / surface->height() : 0.5));
    };
    surface->onClick       = [this] { togglePlay(); };
    surface->onDoubleClick = [this] {
        togglePlay(); // undo the toggle of the first click of the double click
        toggleFullscreen();
    };
    surface->onActivity = [this] { showControls(); };

    connect(helpButton, &QAbstractButton::clicked, this, [this] { toggleShortcuts(); });
    connect(prevButton, &QAbstractButton::clicked, this, [this] { step(-1); });
    connect(nextButton, &QAbstractButton::clicked, this, [this] { step(1); });
    connect(exitButton, &QAbstractButton::clicked, this, [this] { toggleFullscreen(); });
    connect(fsCopy, &QAbstractButton::clicked, this, [this] { copyCurrent(); });
    connect(fsSave, &QAbstractButton::clicked, this, [this] { saveCurrent(); });
    connect(fsOpen, &QAbstractButton::clicked, this, [this] { openCurrent(); });
    connect(playButton, &QAbstractButton::clicked, this, [this] { togglePlay(); });
    connect(muteButton, &QAbstractButton::clicked, this, [this] { toggleMute(); });
    connect(fullscreenButton, &QAbstractButton::clicked, this, [this] { toggleFullscreen(); });
    connect(loopButton, &QAbstractButton::toggled, this, [this](bool on) {
        loop = on;
        if (player)
            player->setLoop(on);
        loopButton->setToolTip(on ? i18n::t("Loop: on (L)") : i18n::t("Loop: off (L)"));
    });
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) { setVolume(value, false); });
    connect(volumeSlider, &QSlider::sliderReleased, this, [this] { setVolume(volumeSlider->value(), true); });

    // Seeking: playback pauses while the handle is held so position updates never fight the drag.
    connect(seekSlider, &QSlider::sliderPressed, this, [this] {
        if (!player || !player->isLoaded())
            return;
        resumeAfterScrub = player->isPlaying();
        if (resumeAfterScrub)
            player->pause();
        scrubClock.start();
        seekTo(seekSlider->value());
    });
    connect(seekSlider, &QSlider::sliderMoved, this, [this](int value) {
        updateTime(value);
        if (player && scrubClock.isValid() && scrubClock.elapsed() > 150) {
            seekTo(value);
            scrubClock.restart();
        }
    });
    connect(seekSlider, &QSlider::sliderReleased, this, [this] {
        if (!player || !player->isLoaded())
            return;
        seekTo(seekSlider->value());
        if (resumeAfterScrub)
            player->play();
        resumeAfterScrub = false;
        showControls();
    });

    connect(messageStore, &QPushButton::clicked, this, [this] { openStore(); });
    connect(messageOpen, &QPushButton::clicked, this, [this] { openCurrent(); });
    connect(messageRetry, &QPushButton::clicked, this, [this] { primaryAction(); });
    connect(fitButton, &QPushButton::clicked, canvas, &ImageCanvas::setFit);
    connect(actualButton, &QPushButton::clicked, canvas, &ImageCanvas::setActualSize);
    connect(actionButton, &QPushButton::clicked, this, [this] { primaryAction(); });
    connect(copyButton, &QPushButton::clicked, this, [this] { copyCurrent(); });
    connect(saveButton, &QPushButton::clicked, this, [this] { saveCurrent(); });
    connect(folderButton, &QPushButton::clicked, this, [this] {
        if (core)
            core->revealInFolder(currentKey());
    });
    connect(openButton, &QPushButton::clicked, this, [this] { openCurrent(); });

    if (core)
        connect(core.data(), &Core::entryChanged, this, [this](const QString& changedKey) { onEntryChanged(changedKey); });
    q->installEventFilter(this);
}

const MediaEntry* MediaViewer::Private::entry() const
{
    return core && index >= 0 && index < keys.size() ? core->entry(keys.at(index)) : nullptr;
}

QString MediaViewer::Private::currentKey() const
{
    return index >= 0 && index < keys.size() ? keys.at(index) : QString();
}

// Size for placeholders (BlurHash / preview): the size the full picture will have on this screen,
// so swapping in the real image never changes what is shown.
QSize MediaViewer::Private::stillTarget() const
{
    QSize bound(1600, 1600);
    if (const QScreen* screen = q->screen())
        bound = screen->availableGeometry().size();
    const MediaEntry* e     = entry();
    const QSize       media = e && e->link.width > 0 && e->link.height > 0 ? QSize(e->link.width, e->link.height) : bound;
    if (media.width() <= bound.width() && media.height() <= bound.height())
        return media;
    return media.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

void MediaViewer::Private::setGallery(const QStringList& newKeys, int newIndex)
{
    QStringList valid;
    for (const QString& key : newKeys) {
        if (core && core->entry(key) && !valid.contains(key))
            valid.append(key);
    }
    if (valid.isEmpty())
        return;

    const QString wanted = newIndex >= 0 && newIndex < newKeys.size() ? newKeys.at(newIndex) : QString();
    const int     target = qMax(0, valid.indexOf(wanted));
    const QString shown  = currentKey();
    keys                 = valid;
    if (valid.contains(wanted))
        requested.insert(wanted); // the user opened the viewer on it

    if (!shown.isEmpty() && keys.at(target) == shown) {
        // Same item (e.g. "expand" on the video that is already open): keep it playing.
        index = target;
        updateTopBar();
        showControls();
        return;
    }
    index = -1;
    showItem(target);
}

void MediaViewer::Private::step(int delta)
{
    const int target = index + delta;
    if (target >= 0 && target < keys.size()) {
        showItem(target);
        return;
    }
    // At either end: say so instead of doing nothing.
    if (keys.size() <= 1)
        flash(i18n::t("No other media in this chat"), Glyph::Info, 1200);
    else
        flash(delta < 0 ? i18n::t("First item in this chat") : i18n::t("Last item in this chat"), Glyph::Info, 1200);
}

void MediaViewer::Private::teardown()
{
    ++generation;
    pool.clear();
    downloadTimer->stop();
    hideTimer->stop();

    if (player) {
        mf::VideoPlayer* p = player;
        player             = nullptr;
        p->disconnect(this);
        delete p; // shuts the engine down synchronously
    }
    if (movie) {
        QMovie* m = movie;
        movie     = nullptr;
        m->disconnect(this);
        m->stop();
        delete m;
    }
    gifAutoPaused = false;
    wasPlaying    = false;
    autoplay      = true;

    content     = Content::None;
    started     = false;
    loaded      = false;
    stillSource = MediaStill::None;
    fullImage   = QImage();
    errorTitle.clear();
    errorDetail.clear();
    storeApp.clear();
    tooLarge         = false;
    tooLargeSize     = QSize();
    playerFailed     = false;
    hasFrame         = false;
    resumeAfterScrub = false;
    pendingSeekMs    = -1;
    frameSize        = QSize();
    if (surface)
        surface->clear();
    if (canvas)
        canvas->setImage(QImage(), true);

    if (!inUseKey.isEmpty()) {
        if (core)
            core->setInUse(inUseKey, false);
        inUseKey.clear();
    }
}

void MediaViewer::Private::showItem(int i)
{
    if (i < 0 || i >= keys.size() || i == index)
        return;
    teardown();
    index                 = i;
    const MediaEntry* e   = entry();
    const QString     key = currentKey();
    if (!e)
        return;

    // A new item starts with the keys on the media (a button reached with Tab may go away now).
    if (q->focusWidget() != q)
        q->setFocus(Qt::OtherFocusReason);

    inUseKey = key;
    core->setInUse(key, true);

    switch (e->kind) {
    case MediaKind::Image:
        // WebP may be animated; startAnimation() falls back to a still image when it is not.
        content = QFileInfo(e->link.fileName).suffix().compare(QLatin1String("webp"), Qt::CaseInsensitive) == 0 ? Content::Animation : Content::Image;
        break;
    case MediaKind::AnimatedImage:
        content = Content::Animation;
        break;
    case MediaKind::Video:
        content = Content::Video;
        break;
    case MediaKind::Audio:
        content = Content::Audio;
        break;
    default:
        content = Content::File;
        break;
    }

    canvas->setVisible(!isPlayable());
    surface->setVisible(isPlayable());
    if (content == Content::Audio)
        surface->setAudioTitle(displayNameFor(e->link));
    applyStill(true);

    {
        const QSignalBlocker blocker(seekSlider);
        seekSlider->setRange(0, 0);
        seekSlider->setValue(0);
    }

    if (e->state == MediaState::Ready)
        startLoading();
    else if (e->state == MediaState::Idle && fetchesAutomatically())
        downloadTimer->start();

    updateTopBar();
    updateTime(0);
    updatePlaybackUi();
    updateOverlay();
    updateButtons();
    showControls();
    layoutStage();
}

void MediaViewer::Private::applyStill(bool reset)
{
    if (!core || content == Content::File || content == Content::Audio)
        return;
    if (!reset && (loaded || hasFrame))
        return;

    const MediaStill still = core->still(currentKey(), stillTarget());
    if (!reset && still.source <= stillSource)
        return;
    stillSource = still.source;
    if (isPicture())
        canvas->setImage(still.image, reset);
    else
        surface->setImage(still.image);
}

void MediaViewer::Private::startLoading()
{
    const MediaEntry* e = entry();
    if (!e || e->state != MediaState::Ready || started)
        return;
    started = true;
    switch (content) {
    case Content::Image:
        loadImage(e->localPath);
        break;
    case Content::Animation:
        startAnimation(e->localPath);
        break;
    case Content::Video:
    case Content::Audio:
        openPlayer(e->localPath);
        break;
    case Content::None:
    case Content::File:
        break;
    }
}

void MediaViewer::Private::loadImage(const QString& path)
{
    const quint64 forGeneration = generation;
    pool.clear();
    pool.start([this, forGeneration, path] {
        const Decoded result = decodeImage(path);
        // Posted to this object: dropped automatically if the viewer is gone by then.
        QMetaObject::invokeMethod(this, [this, forGeneration, result] { onDecoded(forGeneration, result); }, Qt::QueuedConnection);
    });
}

void MediaViewer::Private::onDecoded(quint64 forGeneration, const Decoded& result)
{
    if (forGeneration != generation)
        return;
    if (result.tooLarge) {
        tooLarge     = true;
        tooLargeSize = result.size;
    } else if (result.image.isNull()) {
        errorTitle  = i18n::t("This image can't be shown");
        errorDetail = i18n::t("The file may be damaged or in an unsupported format.");
    } else {
        fullImage = result.image;
        loaded    = true;
        canvas->setImage(fullImage, stillSource == MediaStill::None);
    }
    updateTopBar();
    updateOverlay();
    updateButtons();
}

void MediaViewer::Private::startAnimation(const QString& path)
{
    QImageReader reader(path);
    if (!reader.supportsAnimation() || reader.imageCount() == 1) {
        content = Content::Image; // a single-frame GIF / WebP
        loadImage(path);
        updateTopBar();
        return;
    }
    const QSize size = reader.size();
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaxPixels) {
        tooLarge     = true;
        tooLargeSize = size;
        return;
    }

    movie = new QMovie(path, QByteArray(), this);
    if (!movie->isValid()) {
        delete movie;
        movie   = nullptr;
        content = Content::Image;
        loadImage(path);
        updateTopBar();
        return;
    }
    connect(movie, &QMovie::frameChanged, this, [this] {
        if (!movie)
            return;
        canvas->setImage(movie->currentImage(), false);
        if (!loaded) {
            loaded = true;
            updateTopBar();
            updateOverlay();
            updateButtons();
        }
    });
    movie->start();
}

void MediaViewer::Private::openPlayer(const QString& path)
{
    player = new mf::VideoPlayer(this);
    connect(player, &mf::VideoPlayer::loaded, this, [this] { onPlayerLoaded(); });
    connect(player, &mf::VideoPlayer::failed, this, [this](const QString& error) {
        playerFailed = true;
        storeApp     = storeAppIn(error);
        errorTitle   = content == Content::Audio ? i18n::t("This audio file can't be played here")
                                                 : i18n::t("This video can't be played here");
        // The cause first: it often names the fix. Another app only helps when it doesn't.
        errorDetail = error.isEmpty() ? i18n::t("Windows couldn't play this file.") : error;
        if (!namesFix(error))
            errorDetail += QLatin1Char('\n') + i18n::t("You can still open it with your default app.");
        updatePlaybackUi();
        updateOverlay();
        updateButtons();
        showControls(); // hides the player bar: nothing in it works now
    });
    connect(player, &mf::VideoPlayer::frameReady, this, [this] {
        if (!player)
            return;
        surface->setImage(player->currentFrame());
        if (!hasFrame) {
            hasFrame = true;
            updateButtons();
        }
    });
    connect(player, &mf::VideoPlayer::stateChanged, this, [this] {
        // Every start goes through here (autoplay, Play / Space, replay, resume after scrubbing).
        const bool nowPlaying = player && player->isPlaying();
        const bool begins     = nowPlaying && !wasPlaying;
        wasPlaying            = nowPlaying;
        updatePlaybackUi();
        if (begins)
            emit q->playbackStarted();
    });
    connect(player, &mf::VideoPlayer::videoSizeChanged, this, [this] {
        // The stream switched resolution / aspect: produce frames for the new picture at the size it
        // is shown (the player letterboxes it into the old size until then) and show its dimensions.
        if (!player)
            return;
        frameSize = QSize(); // always hand the player a new request, even if the size happens to match
        updateFrameSize();
        updateTopBar();
    });
    connect(player, &mf::VideoPlayer::positionChanged, this, [this](qint64 ms) {
        if (seekSlider->isSliderDown())
            return;
        if (pendingSeekMs >= 0) {
            // Until the engine reports the new position, ignore the old one so the handle doesn't jump back.
            if (qAbs(ms - pendingSeekMs) > 1500 && pendingSeekClock.elapsed() < 2000)
                return;
            pendingSeekMs = -1;
        }
        {
            const QSignalBlocker blocker(seekSlider);
            seekSlider->setValue(static_cast<int>(qMin<qint64>(ms, seekSlider->maximum())));
        }
        updateTime(ms);
    });
    player->open(path);
}

void MediaViewer::Private::onPlayerLoaded()
{
    if (!player)
        return;
    player->setVolume(volume / 100.0);
    player->setMuted(muted);
    player->setLoop(loop);

    const qint64 duration = player->duration();
    {
        const QSignalBlocker blocker(seekSlider);
        seekSlider->setRange(0, static_cast<int>(qBound<qint64>(0, duration, std::numeric_limits<int>::max())));
        seekSlider->setValue(0);
        seekSlider->setSingleStep(5000);
        seekSlider->setPageStep(10000);
    }
    updateFrameSize();
    if (autoplay)
        player->play();

    updateTopBar();
    updateTime(0);
    updatePlaybackUi();
    updateOverlay();
    updateButtons();
    showControls();
}

void MediaViewer::Private::onEntryChanged(const QString& changedKey)
{
    if (changedKey != currentKey())
        return;
    const MediaEntry* e = entry();
    if (!e)
        return;
    if (e->state == MediaState::Ready && !started)
        startLoading();
    else if (!loaded)
        applyStill(false); // a better placeholder (the preview) may have arrived
    if (e->state == MediaState::Idle && !started && !downloadTimer->isActive() && fetchesAutomatically())
        downloadTimer->start(); // e.g. evicted from the cache while waiting
    if (isPlayable() && !(player && player->isLoaded()))
        updatePlaybackUi(); // the play button of an item waiting for Play goes once its download starts
    updateOverlay();
    updateButtons();
}

void MediaViewer::Private::primaryAction()
{
    const MediaEntry* e = entry();
    if (!core || !e)
        return;
    const QString key = currentKey();
    autoplay          = true; // "Download and play" / Retry: play it once it is there
    if (e->state == MediaState::Failed) {
        requested.insert(key);
        core->retry(key);
    } else if (e->state == MediaState::Idle) {
        requested.insert(key); // before download(): it reports the change synchronously
        core->download(key, false);
    }
}

// Whether the shown item is downloaded without asking. Pictures are: showing them is what the viewer
// is for. Videos and audio can be gigabytes, so only those the user asked for (the item the viewer was
// opened on, or Play / Download pressed) and those the chat downloads automatically too: the same rule
// as Core's automatic downloads (Settings::videoAutoDownloadMB, 0 = "Off (download when played)"; the
// setting is off without inline previews). Files Core stopped because their real size exceeded the
// limit (tooLargeForAuto) and links of unknown size are never fetched unasked: they wait for Play.
bool MediaViewer::Private::fetchesAutomatically() const
{
    const MediaEntry* e = entry();
    if (!e || content == Content::None || content == Content::File)
        return false;
    if (isPicture() || requested.contains(currentKey()))
        return true;
    // 2.2 data saver: videos and audio of a server that saves data wait for Play.
    const Settings s     = Settings::instance().forServer(e->link.serverUid);
    const quint64  limit = static_cast<quint64>(qMax(0, s.videoAutoDownloadMB)) * 1024 * 1024;
    return s.inlinePreviews && limit > 0 && e->link.size > 0 && e->link.size <= limit && !e->tooLargeForAuto && !s.dataSaver;
}

// A video / audio that is not downloaded and is not fetched automatically: it shows its poster with a
// play button that downloads it (and plays it once it is there).
bool MediaViewer::Private::waitingForPlay() const
{
    const MediaEntry* e = entry();
    return isPlayable() && e && e->state == MediaState::Idle && !fetchesAutomatically();
}

// "Open with default app" applies: the file is here, and it isn't a program or script (Core only
// shows those in their folder, it never runs them).
bool MediaViewer::Private::canOpen() const
{
    const MediaEntry* e = entry();
    return core && e && e->state == MediaState::Ready && !core->isUnsafeToOpen(currentKey());
}

// Why Copy / Save / Open can't act on the shown item yet, as a short flash text.
QString MediaViewer::Private::notReadyText() const
{
    const MediaEntry* e = entry();
    if (!e)
        return {};
    switch (e->state) {
    case MediaState::Idle:
        return i18n::t("Not downloaded yet");
    case MediaState::Queued:
        return i18n::t("Waiting to download…");
    case MediaState::Downloading:
        return i18n::t("Still downloading…");
    case MediaState::Failed:
        return downloadErrorTitle(e->error);
    case MediaState::Ready:
        break;
    }
    return i18n::t("Still loading…");
}

// ---- presentation ---------------------------------------------------------------------------

void MediaViewer::Private::updateTopBar()
{
    const MediaEntry* e = entry();
    if (!e)
        return;
    // The name comes from a chat link: shown without control / bidi override characters, and the
    // tooltip (which Qt would read as rich text if it looked like HTML) is escaped.
    const QString name = displayNameFor(e->link);
    displayName        = name;
    q->setWindowTitle(QStringLiteral("%1 — " TSMEDIA_NAME).arg(name));
    // 2.2 drag-out: the second line says the name can be dragged.
    const QString tip = QStringLiteral("<p style='white-space:pre'>%1</p><p>%2</p>")
                            .arg(name.toHtmlEscaped(), i18n::t("Drag the name to copy the file to a folder or app.").toHtmlEscaped());
    nameLabel->setToolTip(tip);
    fsName->setToolTip(tip);
    elideName();

    QStringList parts;
    if (content == Content::Animation)
        parts << QFileInfo(name).suffix().toUpper();

    QSize dims;
    if (!fullImage.isNull())
        dims = fullImage.size();
    else if (player && player->isLoaded() && player->videoSize().isValid())
        dims = player->videoSize();
    else if (e->link.width > 0 && e->link.height > 0)
        dims = QSize(e->link.width, e->link.height);
    if (dims.isValid() && content != Content::Audio)
        parts << QStringLiteral("%1 × %2").arg(dims.width()).arg(dims.height());

    const qint64 duration = player && player->isLoaded() && player->duration() > 0 ? player->duration() : e->link.durationMs;
    if (isPlayable() && duration > 0)
        parts << formatDuration(duration);
    if (e->link.size > 0)
        parts << formatSize(e->link.size);
    metaLabel->setText(parts.join(QStringLiteral("  ·  ")));

    const QString position    = i18n::t("%1 of %2").arg(index + 1).arg(keys.size());
    const QString positionTip = i18n::t("Item %1 of %2 in this chat").arg(index + 1).arg(keys.size());
    for (QLabel* counter : {counterLabel, fsCounter}) {
        counter->setText(position);
        counter->setToolTip(positionTip);
        counter->setVisible(keys.size() > 1);
    }

    // What screen readers say for the picture or video itself.
    for (QWidget* media : {static_cast<QWidget*>(canvas), static_cast<QWidget*>(surface)}) {
        media->setAccessibleName(name);
        media->setAccessibleDescription(metaLabel->text());
    }
}

void MediaViewer::Private::elideName()
{
    nameLabel->setText(nameLabel->fontMetrics().elidedText(displayName, Qt::ElideMiddle, qMax(40, nameLabel->width())));
    fsName->setText(fsName->fontMetrics().elidedText(displayName, Qt::ElideMiddle, qMax(40, fsName->width())));
}

void MediaViewer::Private::updateOverlay()
{
    setPanelButtons(showStatus());
}

// Shows the state of the item over it; returns the buttons its message panel offers.
MediaViewer::Private::PanelButtons MediaViewer::Private::showStatus()
{
    const MediaEntry* e = entry();
    if (!e) {
        overlay->clear();
        return {};
    }
    if (!errorTitle.isEmpty()) {
        overlay->showMessage(Glyph::Error, errorTitle, errorDetail, true);
        // Playing or decoding failed: the file is here, and another app may manage.
        PanelButtons buttons;
        buttons.open  = canOpen();
        buttons.store = playerFailed && !storeApp.isEmpty();
        return buttons;
    }
    if (tooLarge) {
        overlay->showMessage(Glyph::Info, i18n::t("This image is too large to show here"),
                             i18n::t("%1 pixels. Open it with your default app instead.")
                                 .arg(QStringLiteral("%1 × %2").arg(tooLargeSize.width()).arg(tooLargeSize.height())),
                             false);
        PanelButtons buttons;
        buttons.open = canOpen();
        return buttons;
    }
    const bool showing = loaded || (player && player->isLoaded() && !playerFailed);
    if (showing) {
        if (content == Content::Animation && movie && movie->state() == QMovie::Paused)
            overlay->showPaused();
        else
            overlay->clear();
        return {};
    }
    if (e->state == MediaState::Failed) {
        const QString detail = e->errorText.isEmpty() ? downloadErrorText(e->error) : e->errorText;
        overlay->showMessage(Glyph::Error, downloadErrorTitle(e->error), detail, true);
        // Retrying can't bring back a deleted file or get past a channel password (as in the chat).
        PanelButtons buttons;
        buttons.retry = e->error != MediaError::NotFound && e->error != MediaError::Password;
        return buttons;
    }

    auto downloadText = [e] {
        if (e->link.size == 0)
            return i18n::t("Downloading…");
        const quint64 done = static_cast<quint64>(qBound(0.0, e->progress, 1.0) * static_cast<double>(e->link.size));
        return i18n::t("Downloading… %1").arg(formatProgress(done, e->link.size));
    };

    if (content == Content::File) {
        if (e->state == MediaState::Downloading) {
            overlay->showProgress(e->progress, downloadText());
            return {};
        }
        if (e->state == MediaState::Queued) {
            overlay->showProgress(-1, i18n::t("Waiting to download…"));
            return {};
        }
        QString detail = (e->link.size > 0 ? formatSize(e->link.size) + QStringLiteral("  ·  ") : QString()) + i18n::t("No preview available");
        if (e->state == MediaState::Ready && core && core->isUnsafeToOpen(currentKey()))
            detail += QLatin1Char('\n') + i18n::t("Programs and scripts from chat are never run from here."); // why there is no Open
        overlay->showMessage(Glyph::File, displayNameFor(e->link), detail, false);
        return {};
    }

    switch (e->state) {
    case MediaState::Downloading:
        overlay->showProgress(e->progress, downloadText());
        return {};
    case MediaState::Queued:
        overlay->showProgress(-1, i18n::t("Waiting to download…"));
        return {};
    case MediaState::Idle:
        if (waitingForPlay()) {
            const QString text = e->link.size > 0 ? i18n::t("Press play to download (%1)").arg(formatSize(e->link.size))
                                                  : i18n::t("Press play to download");
            // Under the play button; for audio under the file name the surface draws below its icon.
            overlay->showPrompt(text, content == Content::Audio ? 100 : kCenterButton + 14);
        } else {
            overlay->showProgress(-1, i18n::t("Waiting to download…")); // starts after kDownloadDelay
        }
        return {};
    case MediaState::Ready:
    case MediaState::Failed:
        break;
    }

    if (isPlayable() && player && !player->isLoaded() && !playerFailed) {
        overlay->showProgress(-1, QString());
        return {};
    }
    if (isPicture() && started && stillSource < MediaStill::Preview) {
        overlay->showProgress(-1, QString());
        return {};
    }
    overlay->clear();
    return {};
}

void MediaViewer::Private::setPanelButtons(const PanelButtons& buttons)
{
    setShown(messageStore, buttons.store);
    setShown(messageOpen, buttons.open);
    setShown(messageRetry, buttons.retry);
    if (buttons.store)
        messageStore->setToolTip(i18n::t("Find “%1” in the Microsoft Store").arg(storeApp));
    // One main action: the fix the message names comes before the fallback.
    setPrimary(messageStore, buttons.store);
    setPrimary(messageOpen, buttons.open && !buttons.store);
    setPrimary(messageRetry, buttons.retry);
    placeMessageActions();
}

void MediaViewer::Private::placeMessageActions()
{
    const bool any = !messageStore->isHidden() || !messageOpen->isHidden() || !messageRetry->isHidden();
    if (!any || !overlay->showsMessage()) {
        setShown(messageActions, false);
        overlay->setActionsSize(QSize());
        return;
    }
    // Side by side, or one under the other when the window is too narrow.
    messageLayout->setDirection(QBoxLayout::LeftToRight);
    if (messageLayout->sizeHint().width() > stage->width() - 64)
        messageLayout->setDirection(QBoxLayout::TopToBottom);
    overlay->setActionsSize(messageLayout->sizeHint());
    const QRect area = overlay->actionsRect();
    if (area.isEmpty()) {
        setShown(messageActions, false);
        return;
    }
    messageActions->setGeometry(area);
    messageActions->show();
}

void MediaViewer::Private::updateButtons()
{
    const MediaEntry* e      = entry();
    const bool        ready  = e && e->state == MediaState::Ready;
    const bool        unsafe = ready && core && core->isUnsafeToOpen(currentKey());

    fitButton->setVisible(isPicture());
    actualButton->setVisible(isPicture());
    zoomLabel->setVisible(isPicture() && loaded);
    fitButton->setEnabled(loaded);
    actualButton->setEnabled(loaded);
    updateZoomButtons();

    const bool canCopy = (content == Content::Image && !fullImage.isNull()) || (content == Content::Animation && loaded && movie) || (content == Content::Video && hasFrame);
    setShown(copyButton, isPicture() || content == Content::Video);
    copyButton->setEnabled(canCopy);
    copyButton->setText(content == Content::Image ? i18n::t("Copy image") : i18n::t("Copy frame")); // an animation copies the frame shown
    saveButton->setEnabled(ready);
    folderButton->setEnabled(ready);
    // Programs and scripts from chat are never run (Core would only show them in their folder).
    setShown(openButton, !unsafe);
    openButton->setEnabled(ready);

    // Retry is in the message panel, next to the explanation (showStatus).
    if (e && content == Content::File && e->state == MediaState::Idle) {
        actionButton->setText(i18n::t("Download"));
        actionButton->setToolTip(i18n::t("Download the file (Enter)"));
        setShown(actionButton, true);
    } else if (waitingForPlay()) {
        actionButton->setText(i18n::t("Download and play"));
        actionButton->setToolTip(i18n::t("Download and play (Enter)"));
        setShown(actionButton, true);
    } else {
        setShown(actionButton, false);
    }
    // One highlighted button. For pictures and videos, viewing them here is the main task; for other
    // files it is opening them (or, for programs, finding them).
    const bool fileAction = content == Content::File && actionButton->isHidden();
    setPrimary(openButton, fileAction && !unsafe);
    setPrimary(folderButton, fileAction && unsafe);

    // The full-screen header offers the same.
    fsCopy->setVisible(!copyButton->isHidden());
    fsCopy->setEnabled(canCopy);
    setIconAction(fsCopy, copyButton->text(), i18n::t("%1 (Ctrl+C)").arg(copyButton->text()));
    fsSave->setEnabled(ready);
    fsOpen->setVisible(!unsafe);
    fsOpen->setEnabled(ready);
}

// Fit and 100% show which view is active; both do when the picture fits at 100%.
void MediaViewer::Private::updateZoomButtons()
{
    const bool fit    = loaded && canvas->isFit();
    const bool actual = loaded && qAbs(canvas->zoom() - 1.0) < 0.001;
    fitButton->setChecked(fit);
    actualButton->setChecked(actual);
    const QString tip = fit && actual ? i18n::t("Already shown at actual size (1)") : i18n::t("Actual size: one image pixel per screen pixel (1)");
    if (actualButton->toolTip() != tip)
        actualButton->setToolTip(tip);
}

void MediaViewer::Private::updatePlaybackUi()
{
    if (!isPlayable())
        return;
    const bool ready    = player && player->isLoaded() && !playerFailed;
    const bool playing  = ready && player->isPlaying();
    const bool ended    = ready && player->isEnded();
    const bool awaiting = !ready && waitingForPlay();

    playButton->setEnabled(ready || awaiting);
    seekSlider->setEnabled(ready && seekSlider->maximum() > 0);
    playButton->setGlyph(playing ? Glyph::Pause : ended ? Glyph::Replay : Glyph::Play);
    const QString play = playing    ? i18n::t("Pause")
                         : ended    ? i18n::t("Replay")
                         : awaiting ? i18n::t("Download and play")
                                    : i18n::t("Play");
    setIconAction(playButton, play, i18n::t("%1 (Space)").arg(play));

    const bool silent = muted || volume == 0;
    muteButton->setGlyph(silent ? Glyph::VolumeMuted : volume < 50 ? Glyph::VolumeLow : Glyph::VolumeHigh);
    const QString mute = silent ? i18n::t("Unmute") : i18n::t("Mute");
    setIconAction(muteButton, mute, i18n::t("%1 (M)").arg(mute));
    {
        const QSignalBlocker blocker(volumeSlider);
        volumeSlider->setValue(muted ? 0 : volume);
    }

    surface->setCenter(ready && !playing ? (ended ? VideoSurface::Center::Replay : VideoSurface::Center::Play)
                       : awaiting        ? VideoSurface::Center::Play
                                         : VideoSurface::Center::None);
    if (ready && !seekSlider->isSliderDown() && pendingSeekMs < 0)
        updateTime(player->position());
    if (!playing)
        seekSlider->setAccessibleDescription(timeLabel->text()); // not on every position update
    showControls();
}

void MediaViewer::Private::updateTime(qint64 positionMs)
{
    const MediaEntry* e        = entry();
    const qint64      duration = player && player->isLoaded() && player->duration() > 0 ? player->duration() : (e ? e->link.durationMs : 0);
    const QString     total    = formatDuration(qMax<qint64>(0, duration));
    timeLabel->setText(formatDuration(qBound<qint64>(0, positionMs, qMax(positionMs, duration))) + QStringLiteral(" / ") + total);
    // Reserve the widest text for this duration so the seek bar doesn't wobble while playing.
    timeLabel->setMinimumWidth(timeLabel->fontMetrics().horizontalAdvance(total + QStringLiteral(" / ") + total) + 4);
}

void MediaViewer::Private::layoutStage()
{
    const QRect r = stage->rect();
    canvas->setGeometry(r);
    surface->setGeometry(r);
    overlay->setGeometry(r);
    sheet->setGeometry(r);

    const int nav    = 44;
    const int margin = 16;
    const int navY   = (r.height() - nav) / 2;
    prevButton->setGeometry(margin, navY, nav, nav);
    nextButton->setGeometry(r.width() - margin - nav, navY, nav, nav);

    const int barHeight = controls->sizeHint().height();
    controls->setGeometry(0, r.height() - barHeight, r.width(), barHeight);
    fsHeader->setGeometry(0, 0, r.width(), fsHeader->sizeHint().height());

    placeMessageActions();
    placeFlash();
    updateFrameSize();
}

// Top centre of the stage, below the full-screen header while that is shown.
void MediaViewer::Private::placeFlash()
{
    const int top = fsHeader->isVisible() ? fsHeader->height() - 12 : 20;
    flashBubble->move((stage->width() - flashBubble->width()) / 2, top);
}

// Frames are produced at the size they are shown (device pixels), never above the video's own size.
void MediaViewer::Private::updateFrameSize()
{
    if (!player || !player->isLoaded())
        return;
    const QSize video = player->videoSize();
    if (video.isEmpty())
        return;
    const QSize box    = (QSizeF(surface->size()) * surface->devicePixelRatioF()).toSize();
    QSize       target = video.scaled(box.expandedTo(QSize(2, 2)), Qt::KeepAspectRatio);
    if (target.width() > video.width() || target.height() > video.height())
        target = video;
    target = QSize(qMax(2, target.width() & ~1), qMax(2, target.height() & ~1));
    if (target != frameSize) {
        frameSize = target;
        player->setFrameSize(target);
    }
}

void MediaViewer::Private::showControls()
{
    stopFade(); // any movement interrupts a fade-out
    const bool fullscreen = q->isFullScreen();
    controls->setVisible(isPlayable() && !playerFailed);
    const bool gallery = keys.size() > 1;
    prevButton->setVisible(gallery && index > 0);
    nextButton->setVisible(gallery && index < keys.size() - 1);
    fsHeader->setVisible(fullscreen);
    surface->setCursor(Qt::ArrowCursor);
    canvas->setCursorHidden(false);
    placeFlash();
    // Hidden while a video plays, and in full screen for everything but a paused video (whose bar
    // stays, as in other players).
    const bool playing = isPlayable() && player && player->isPlaying();
    if (playing || (fullscreen && !isPlayable()))
        hideTimer->start();
    else
        hideTimer->stop();
}

void MediaViewer::Private::autoHideControls()
{
    const bool playing = isPlayable() && player && player->isPlaying();
    if (!playing && !(q->isFullScreen() && !isPlayable()))
        return;
    const bool interacting = controls->underMouse() || prevButton->underMouse() || nextButton->underMouse() || fsHeader->underMouse()
                             || messageActions->underMouse() || seekSlider->isSliderDown() || volumeSlider->isSliderDown() || sheet->isVisible();
    if (interacting) {
        hideTimer->start();
        return;
    }
    QList<QWidget*> visible;
    for (QWidget* w : {static_cast<QWidget*>(controls), static_cast<QWidget*>(prevButton), static_cast<QWidget*>(nextButton), static_cast<QWidget*>(fsHeader)}) {
        if (w->isVisible())
            visible << w;
    }
    fadeOut(visible);
    surface->setCursor(Qt::BlankCursor);
    canvas->setCursorHidden(true);
}

void MediaViewer::Private::fadeOut(const QList<QWidget*>& widgets)
{
    stopFade();
    if (widgets.isEmpty())
        return;
    if (!ui::animationsEnabled()) {
        for (QWidget* w : widgets)
            w->hide();
        return;
    }
    for (QWidget* w : widgets) {
        w->setGraphicsEffect(new QGraphicsOpacityEffect(w)); // the widget owns it
        fading << w;
    }
    fade->start();
}

// Ends a fade-out early; the widgets keep whatever visibility they have.
void MediaViewer::Private::stopFade()
{
    if (fading.isEmpty())
        return;
    fade->stop();
    const QList<QPointer<QWidget>> faded = fading;
    fading.clear();
    for (const QPointer<QWidget>& w : faded) {
        if (w)
            w->setGraphicsEffect(nullptr);
    }
}

void MediaViewer::Private::onWindowStateChanged()
{
    const bool fullscreen = q->isFullScreen();
    topBar->setVisible(!fullscreen);
    actionBar->setVisible(!fullscreen);
    fullscreenButton->setGlyph(fullscreen ? Glyph::FullscreenExit : Glyph::FullscreenEnter);
    setIconAction(fullscreenButton, fullscreen ? i18n::t("Exit full screen") : i18n::t("Full screen"),
                  fullscreen ? i18n::t("Exit full screen (F)") : i18n::t("Full screen (F)"));
    if (q->isMinimized() && player && player->isPlaying())
        player->pause();
    // An animation keeps decoding frames nobody sees while minimized; it resumes on restore unless
    // the user had paused it.
    if (movie) {
        if (q->isMinimized() && movie->state() == QMovie::Running) {
            movie->setPaused(true);
            gifAutoPaused = true;
        } else if (!q->isMinimized() && gifAutoPaused) {
            gifAutoPaused = false;
            if (movie->state() == QMovie::Paused)
                movie->setPaused(false);
        }
        updateOverlay();
    }
    showControls();
    layoutStage();
}

// Hiding a focused button would move the focus to the next one in the Tab order, and Space would
// then press that: the keys go back to the media instead.
void MediaViewer::Private::setShown(QWidget* widget, bool shown)
{
    if (!shown && (q->focusWidget() == widget || widget->isAncestorOf(q->focusWidget())))
        q->setFocus(Qt::OtherFocusReason);
    widget->setVisible(shown);
}

// ---- actions ----------------------------------------------------------------------------------

void MediaViewer::Private::togglePlay()
{
    autoplay = true; // pressed while the file is still downloading / opening: play it once it is loaded
    if (waitingForPlay()) {
        primaryAction(); // downloads it; it starts playing once it is there
        return;
    }
    if (!player || !player->isLoaded() || playerFailed)
        return;
    if (player->isEnded()) {
        player->seek(0);
        player->play();
    } else {
        player->togglePlay();
    }
    showControls();
}

void MediaViewer::Private::toggleGifPause()
{
    if (!movie)
        return;
    gifAutoPaused = false;
    movie->setPaused(movie->state() != QMovie::Paused);
    updateOverlay();
}

void MediaViewer::Private::seekTo(qint64 ms)
{
    if (!player || !player->isLoaded())
        return;
    const qint64 duration = player->duration();
    const qint64 target   = duration > 0 ? qBound<qint64>(0, ms, duration) : qMax<qint64>(0, ms);
    player->seek(target);
    pendingSeekMs = target;
    pendingSeekClock.start();
    {
        const QSignalBlocker blocker(seekSlider);
        seekSlider->setValue(static_cast<int>(qMin<qint64>(target, seekSlider->maximum())));
    }
    updateTime(target);
    seekSlider->setAccessibleDescription(timeLabel->text());
}

void MediaViewer::Private::seekBy(qint64 deltaMs)
{
    if (!player || !player->isLoaded())
        return;
    seekTo((pendingSeekMs >= 0 ? pendingSeekMs : player->position()) + deltaMs);
    showControls();
}

void MediaViewer::Private::setVolume(int percent, bool persist)
{
    volume = qBound(0, percent, 100);
    setMuted(muted && volume == 0, true); // raising the volume unmutes; applies the volume as well
    if (persist) {
        // Remembered for the next video, inline or here.
        Settings& s = Settings::instance();
        if (s.videoVolume != volume) {
            s.videoVolume = volume;
            s.save();
        }
    }
}

// notify: the user changed it here, so the chat's inline players follow (MediaViewer::mutedChanged).
void MediaViewer::Private::setMuted(bool on, bool notify)
{
    const bool changed = on != muted;
    muted              = on;
    if (player) {
        player->setVolume(volume / 100.0);
        player->setMuted(muted);
    }
    updatePlaybackUi();
    if (changed && notify)
        emit q->mutedChanged(muted);
}

void MediaViewer::Private::toggleMute()
{
    if (muted || volume == 0) {
        if (volume == 0)
            volume = 50;
        setMuted(false, true);
    } else {
        setMuted(true, true);
    }
}

void MediaViewer::Private::toggleFullscreen()
{
    if (q->isFullScreen()) {
        if (wasMaximized)
            q->showMaximized();
        else
            q->showNormal();
    } else {
        wasMaximized = q->isMaximized();
        q->showFullScreen();
    }
}

void MediaViewer::Private::toggleShortcuts()
{
    if (sheet->isVisible()) {
        sheet->hide();
        return;
    }
    flashBubble->hide();
    sheet->setGeometry(stage->rect());
    sheet->show();
    sheet->raise();
    QAccessibleEvent shown(sheet, QAccessible::Alert);
    QAccessible::updateAccessibility(&shown);
}

void MediaViewer::Private::copyCurrent()
{
    if (!isPicture() && content != Content::Video) {
        flash(i18n::t("Nothing to copy"), Glyph::Info, 1500);
        return;
    }
    QImage image;
    if (content == Content::Image)
        image = fullImage;
    else if (content == Content::Animation && movie)
        image = movie->currentImage();
    else if (content == Content::Video && player && hasFrame)
        image = player->currentFrame().convertToFormat(QImage::Format_RGB32);
    if (image.isNull()) {
        flash(!errorTitle.isEmpty() || tooLarge ? i18n::t("Nothing to copy") : notReadyText(), Glyph::Info, 1500);
        return;
    }
    QApplication::clipboard()->setImage(image);
    flash(content == Content::Image ? i18n::t("Image copied") : i18n::t("Frame copied"), Glyph::Check, 2500);
}

// 2.2 drag-out
void MediaViewer::Private::startDragOut(const QPointF& pressFraction)
{
    const MediaEntry* e = entry();
    if (!core || !e)
        return;
    const QString key = currentKey();
    if (e->state == MediaState::Idle)
        requested.insert(key); // the drag downloads it (as Download would): show its progress, not "press play"
    // The drag runs a nested event loop: the viewer may be closed meanwhile.
    const QPointer<MediaViewer> guard(q);
    const filedrag::Result      result = filedrag::start(core, key, q, pressFraction, q->devicePixelRatioF(), q->font());
    if (guard && !result.message.isEmpty())
        flash(result.message, Glyph::Info, result.error ? 4000 : 2500);
}

void MediaViewer::Private::copyFileCurrent()
{
    const MediaEntry* e = entry();
    if (!core || !e)
        return;
    if (e->state != MediaState::Ready) {
        flash(notReadyText(), Glyph::Info, 1500);
        return;
    }
    QString    feedback;
    const bool ok = filedrag::copyToClipboard(core, currentKey(), &feedback);
    flash(feedback, ok ? Glyph::Check : Glyph::Info, ok ? 2500 : 4000);
}

void MediaViewer::Private::saveCurrent()
{
    const MediaEntry* e = entry();
    if (!core || !e)
        return;
    if (e->state != MediaState::Ready) {
        flash(notReadyText(), Glyph::Info, 1500);
        return;
    }
    // The dialog runs an event loop: the viewer may be closed meanwhile.
    const QPointer<MediaViewer> guard(q);
    const QString               path = core->saveAs(currentKey(), q);
    if (guard && !path.isEmpty())
        flash(ui::savedToText(path), Glyph::Check, 4000); // a failure was explained by a message box
}

void MediaViewer::Private::openCurrent()
{
    const MediaEntry* e = entry();
    if (!core || !e)
        return;
    if (e->state != MediaState::Ready) {
        flash(notReadyText(), Glyph::Info, 1500);
        return;
    }
    if (player && player->isPlaying())
        player->pause();
    core->openExternally(currentKey());
    if (core->isUnsafeToOpen(currentKey()))
        flash(i18n::t("Shown in folder: programs and scripts from chat are never run"), Glyph::Info, 4000);
}

void MediaViewer::Private::openStore()
{
    if (storeApp.isEmpty())
        return;
    // The Store app opens on its search for the extension the error names.
    QDesktopServices::openUrl(QUrl(QString::fromLatin1("ms-windows-store://search/?query=") + QString::fromLatin1(QUrl::toPercentEncoding(storeApp))));
}

// Enter: the focused button, otherwise the highlighted one (the window's main action), if any.
void MediaViewer::Private::activateDefault()
{
    if (auto* focused = qobject_cast<QPushButton*>(QApplication::focusWidget())) {
        if (focused->window() == q && focused->isVisible() && focused->isEnabled()) {
            focused->animateClick();
            return;
        }
    }
    const bool panel = !messageActions->isHidden();
    for (QPushButton* b : {messageStore, messageOpen, messageRetry, actionButton, openButton, folderButton}) {
        if (b->parentWidget() == messageActions && !panel)
            continue;
        if (b->objectName() == QLatin1String("primary") && !b->isHidden() && b->isEnabled()) {
            b->animateClick(); // also when the bar is hidden in full screen
            return;
        }
    }
}

// The controls may be hidden (playing, full screen): keys that change something say what they did.
void MediaViewer::Private::flash(const QString& text, std::optional<Glyph> glyph, int ms)
{
    if (text.isEmpty())
        return;
    flashBubble->showText(text, glyph, ms, qMax(80, stage->width() - 48));
    placeFlash();
}

void MediaViewer::Private::flashVolume(bool muteKey)
{
    const bool silent = muted || volume == 0;
    if (silent)
        flash(i18n::t("Muted"), Glyph::VolumeMuted, 1000);
    else
        flash(muteKey ? i18n::t("Unmuted") : i18n::t("Volume %1%").arg(volume), volume < 50 ? Glyph::VolumeLow : Glyph::VolumeHigh, 1000);
}

bool MediaViewer::Private::handleKey(QKeyEvent* event)
{
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl  = event->modifiers().testFlag(Qt::ControlModifier);

    // With a non-Latin keyboard layout the letter keys produce other characters;
    // fall back to the physical key so F, M, K, L, 0, 1, Ctrl+C, Ctrl+S and Ctrl+O keep working.
    int           key = event->key();
    const quint32 vk  = event->nativeVirtualKey();
    if (key > 0x7e && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')))
        key = static_cast<int>(vk);
    if (vk == kVkOemPlus || vk == kVkAdd)
        key = Qt::Key_Plus; // = / + on the main keyboard and the number pad, whatever the layout
    else if (vk == kVkOemMinus || vk == kVkSubtract)
        key = Qt::Key_Minus;
    else if (key > 0x7e && vk == kVkOem2 && shift)
        key = Qt::Key_Question;

    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
        return false;
    default:
        break;
    }

    // The shortcut list closes on any key; Esc, ? and F1 only close it.
    if (sheet->isVisible()) {
        sheet->hide();
        if (key == Qt::Key_Escape || key == Qt::Key_Question || key == Qt::Key_F1)
            return true;
    }

    switch (key) {
    case Qt::Key_Escape:
        if (q->isFullScreen())
            toggleFullscreen();
        else
            q->close();
        return true;
    case Qt::Key_Question:
    case Qt::Key_F1:
        toggleShortcuts();
        return true;
    case Qt::Key_Space:
    case Qt::Key_K:
        if (isPlayable())
            togglePlay();
        else if (content == Content::Animation)
            toggleGifPause();
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const int direction = key == Qt::Key_Left ? -1 : 1;
        // Shift seeks; so do plain arrows when this video is the chat's only item (nothing to move to).
        if (isPlayable() && (shift || (keys.size() == 1 && player && player->isLoaded())))
            seekBy(direction * 5000);
        else if (shift && isPicture() && canvas->canPan())
            canvas->panBy(QPointF(-direction * stage->width() * 0.15, 0));
        else
            step(direction);
        return true;
    }
    case Qt::Key_Home:
        if (isPlayable())
            seekTo(0);
        return true;
    case Qt::Key_F:
        toggleFullscreen();
        return true;
    case Qt::Key_M:
        if (isPlayable()) {
            toggleMute();
            flashVolume(true);
        }
        return true;
    case Qt::Key_L:
        if (isPlayable()) {
            loopButton->toggle();
            flash(loop ? i18n::t("Loop on") : i18n::t("Loop off"), Glyph::Loop, 1200);
        }
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int direction = key == Qt::Key_Up ? 1 : -1;
        if (isPlayable()) {
            setVolume((muted ? 0 : volume) + direction * 5, true);
            flashVolume(false);
        } else if (isPicture() && canvas->canPan()) {
            canvas->panBy(QPointF(0, direction * stage->height() * 0.15));
        }
        return true;
    }
    case Qt::Key_0:
        if (isPicture() && loaded)
            canvas->setFit(); // Ctrl+0 as well
        return true;
    case Qt::Key_1:
        if (isPicture() && loaded)
            canvas->setActualSize();
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
    case Qt::Key_Minus:
        if (isPicture() && loaded)
            canvas->zoomBy(key == Qt::Key_Minus ? 0.8 : 1.25);
        return true;
    case Qt::Key_C:
        if (!ctrl)
            return false;
        if (shift) // 2.2 drag-out: the file itself, for Ctrl+V in a folder
            copyFileCurrent();
        else
            copyCurrent();
        return true;
    case Qt::Key_S:
        if (!ctrl)
            return false;
        saveCurrent();
        return true;
    case Qt::Key_O:
        if (!ctrl)
            return false;
        openCurrent();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        activateDefault();
        return true;
    default:
        return false;
    }
}

// ---- MediaViewer --------------------------------------------------------------------------------

void MediaViewer::open(Core* core, const QStringList& keys, int index)
{
    openViewer(core, keys, index, std::nullopt);
}

MediaViewer* MediaViewer::open(Core* core, const QStringList& keys, int index, bool muted)
{
    return openViewer(core, keys, index, muted);
}

void MediaViewer::pausePlayback()
{
    d->resumeAfterScrub = false; // a held seek handle must not restart it on release
    d->autoplay         = false; // a video still downloading / opening does not start by itself later
    if (d->player && d->player->isPlaying())
        d->player->pause(); // stateChanged updates the controls
}

MediaViewer* MediaViewer::openViewer(Core* core, const QStringList& keys, int index, std::optional<bool> muted)
{
    if (!core || keys.isEmpty())
        return nullptr;
    bool any = false;
    for (const QString& key : keys)
        any = any || core->entry(key);
    if (!any)
        return nullptr;

    if (g_viewer && g_viewer->isVisible()) {
        if (muted)
            g_viewer->d->setMuted(*muted, false); // before setGallery: a new item's player starts with it
        g_viewer->d->setGallery(keys, index);
        if (g_viewer->isMinimized())
            g_viewer->showNormal();
        g_viewer->raise();
        g_viewer->activateWindow();
        return g_viewer;
    }

    QWidget* parent = QApplication::activeWindow();
    if (parent && (parent->windowType() == Qt::Popup || parent->windowType() == Qt::ToolTip))
        parent = parent->parentWidget() ? parent->parentWidget()->window() : nullptr;

    auto* viewer = new MediaViewer(core, keys, index, muted, parent);
    g_viewer     = viewer;
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
    viewer->setFocus();
    return viewer;
}

MediaViewer::MediaViewer(Core* core, const QStringList& keys, int index, std::optional<bool> muted, QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowMinimizeButtonHint)
    , d(new Private(this, core))
{
    if (muted)
        d->muted = *muted; // instead of Settings::videosStartMuted
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QString::fromLatin1("tsmediaMediaViewer"));
    setLayoutDirection(Qt::LeftToRight);
    setFocusPolicy(Qt::StrongFocus);
    // fromLatin1, never QStringLiteral: a window with a parent shares the parent's QStyleSheetStyle when
    // TeamSpeak uses style sheets, and its parser keeps the last text it parsed until TeamSpeak destroys
    // it at exit, after this DLL is unloaded (~QString on literal data in the unloaded DLL = crash).
    // The rules name the viewer's own containers: a message box or file dialog opened over the viewer
    // is its child as well, and keeps TeamSpeak's look (light text on a light box otherwise).
    setStyleSheet(QString::fromLatin1(
        "#tsmediaMediaViewer { background: #1e1f22; }"
        "#tsmediaTopBar QLabel, #tsmediaStage QLabel, #tsmediaActionBar QLabel { color: #b5bac1; background: transparent; }"
        "#tsmediaTopBar QLabel#tsmediaTitle { color: #f2f3f5; font-weight: bold; }"
        "#tsmediaTopBar QLabel#tsmediaCounter { color: #dbdee1; background: rgba(255,255,255,20); border-radius: 10px; padding: 3px 10px; }"
        "#tsmediaStage QLabel#tsmediaTime { color: #ffffff; }"
        "#tsmediaStage QLabel#tsmediaFsTitle { color: #ffffff; font-weight: bold; }"
        "#tsmediaStage QLabel#tsmediaFsCounter { color: #ffffff; background: rgba(0,0,0,150); border-radius: 10px; padding: 3px 10px; }"
        // The transparent 2 px border becomes the keyboard focus ring: same size with and without it.
        // #c9cdfb is 5.2:1 on the button and 10.7:1 on the bar; white on the accent button is 4.6:1.
        "#tsmediaActionBar QPushButton, #tsmediaStage QPushButton { color: #f2f3f5; background: #4e5058; border: 2px solid transparent; border-radius: 4px; padding: 4px 10px; outline: none; }"
        "#tsmediaActionBar QPushButton:hover, #tsmediaStage QPushButton:hover { background: #6d6f78; }"
        "#tsmediaActionBar QPushButton:pressed, #tsmediaStage QPushButton:pressed { background: #404249; }"
        "#tsmediaActionBar QPushButton:checked { color: #1e1f22; background: #dbdee1; }"
        "#tsmediaActionBar QPushButton:checked:hover { background: #ffffff; }"
        "#tsmediaActionBar QPushButton:focus, #tsmediaStage QPushButton:focus { border-color: #c9cdfb; }"
        "#tsmediaActionBar QPushButton:disabled, #tsmediaStage QPushButton:disabled { color: #80848e; background: #3a3c42; }"
        "#tsmediaActionBar QPushButton#primary, #tsmediaStage QPushButton#primary { color: #ffffff; background: #5865f2; }"
        "#tsmediaActionBar QPushButton#primary:hover, #tsmediaStage QPushButton#primary:hover { background: #4752c4; }"
        "#tsmediaActionBar QPushButton#primary:pressed, #tsmediaStage QPushButton#primary:pressed { background: #3c45a5; }"
        "#tsmediaActionBar QPushButton#primary:focus, #tsmediaStage QPushButton#primary:focus { border-color: #ffffff; }"
        "#tsmediaActionBar QPushButton#primary:disabled, #tsmediaStage QPushButton#primary:disabled { color: #a3a6aa; background: #3c4270; }"
        "#tsmediaStage QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,70); border-radius: 2px; }"
        "#tsmediaStage QSlider::sub-page:horizontal { background: #5865f2; border-radius: 2px; }"
        "#tsmediaStage QSlider::add-page:horizontal { background: rgba(255,255,255,70); border-radius: 2px; }"
        "#tsmediaStage QSlider::handle:horizontal { background: #ffffff; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }"
        "#tsmediaStage QSlider::handle:horizontal:hover { width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; }"
        "#tsmediaStage QSlider::sub-page:horizontal:disabled { background: rgba(255,255,255,40); }"
        "#tsmediaStage QSlider::handle:horizontal:disabled { background: rgba(255,255,255,90); }"
        "QToolTip { color: #f2f3f5; background: #111214; border: 1px solid #3f4147; padding: 4px 6px; }"));

    d->buildUi();
    d->setGallery(keys, index);

    // Size the window around the first item, within 85% of the screen.
    const QScreen*    screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize       avail  = screen ? screen->availableGeometry().size() * 0.85 : QSize(1280, 800);
    const MediaEntry* e      = d->entry();
    const bool        sized  = e && e->link.width > 0 && e->link.height > 0;
    QSize             media  = sized ? QSize(e->link.width, e->link.height) : QSize(960, 600);
    // A picture is shown at most one image pixel per screen pixel: fewer logical pixels on a scaled display.
    if (sized && d->isPicture() && screen && screen->devicePixelRatio() > 1.0)
        media = (QSizeF(media) / screen->devicePixelRatio()).toSize();
    const QSize chrome(24, 140);
    if (sized && (media.width() + chrome.width() > avail.width() || media.height() + chrome.height() > avail.height()))
        media = media.scaled(avail - chrome, Qt::KeepAspectRatio);
    resize((media + chrome).expandedTo(QSize(720, 520)).boundedTo(avail));

    useDarkTitleBar(this); // creates the native window, so windowHandle() exists from here on

    // Moved to a monitor with another scale: video frames are requested at the new device-pixel size,
    // and a fitted picture fits again (its zoom counts device pixels). Once the window has the new scale.
    if (QWindow* window = windowHandle()) {
        connect(window, &QWindow::screenChanged, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (d->canvas->isFit())
                    d->canvas->setFit();
                d->layoutStage();
                d->canvas->update();
            });
        });
    }
}

MediaViewer::~MediaViewer()
{
    d->teardown();
    delete d;
}

void MediaViewer::keyPressEvent(QKeyEvent* event)
{
    if (d->handleKey(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void MediaViewer::closeEvent(QCloseEvent* event)
{
    d->teardown(); // stop the video right away, not when the deferred delete happens
    QDialog::closeEvent(event);
}

// ============================================================================================
// Windows: dark title bar to match the dark window (Windows 10 20H1+ / 11; ignored elsewhere)
// ============================================================================================

#include <windows.h>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace {
void useDarkTitleBar(QWidget* window)
{
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19 /* same attribute on builds before 20H1 */, &dark, sizeof(dark));
}
} // namespace
