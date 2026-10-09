#include "mediaviewer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBasicTimer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
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
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <limits>

#include "core.h"
#include "i18n.h"
#include "previewrenderer.h"
#include "settings.h"
#include "version.h"
#include "video/mfvideo.h"

namespace {

constexpr qint64 kMaxPixels      = 80LL * 1000 * 1000; // never decode more than this
constexpr int    kHideControlsMs = 2500;
constexpr int    kDownloadDelay  = 250; // flipping quickly through the gallery does not start downloads
constexpr qreal  kCenterButton   = 34;  // radius of the big play button over a video

QColor accentColor()
{
    return QColor(0x58, 0x65, 0xf2);
}

void useDarkTitleBar(QWidget* window); // Windows only, defined at the end of this file

bool canPan(const QImage& image, qreal zoom, const QSize& area)
{
    if (image.isNull())
        return false;
    const QSizeF scaled = QSizeF(image.size()) * zoom;
    return scaled.width() > area.width() + 0.5 || scaled.height() > area.height() + 0.5;
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
    File,
    Music,
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

    void paintEvent(QPaintEvent*) override
    {
        QPainter   p(this);
        const bool hover = underMouse() && isEnabled();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        if (m_round) {
            p.setBrush(QColor(0, 0, 0, isDown() ? 200 : hover ? 170 : 115));
            p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        } else if (hover || isDown()) {
            p.setBrush(QColor(255, 255, 255, isDown() ? 45 : 26));
            p.drawRoundedRect(QRectF(rect()), 6, 6);
        }

        QColor color = QColor(0xdb, 0xde, 0xe1);
        if (!isEnabled())
            color = QColor(0x80, 0x84, 0x8e);
        else if (isCheckable() && isChecked())
            color = accentColor().lighter(hover ? 125 : 112);
        else if (hover || m_round)
            color = Qt::white;

        const qreal inset = width() * (m_round ? 0.24 : 0.2);
        drawGlyph(p, m_glyph, QRectF(rect()).adjusted(inset, inset, -inset, -inset), color);
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
        setFixedHeight(18);
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->pos())) {
                const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
                const int   pos    = event->pos().x() - groove.x() - handle.width() / 2;
                const int   span   = qMax(1, groove.width() - handle.width());
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), pos, span, opt.upsideDown));
            }
        }
        QSlider::mousePressEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (m_wheel)
            QSlider::wheelEvent(event);
        else
            event->ignore();
    }

  private:
    bool m_wheel;
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
        if (event->button() == Qt::LeftButton)
            m_pressPos = event->pos();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !rect().contains(event->pos()))
            return;
        if ((event->pos() - m_pressPos).manhattanLength() <= QApplication::startDragDistance() && onClick)
            onClick();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && onDoubleClick)
            onDoubleClick();
    }

    void mouseMoveEvent(QMouseEvent*) override
    {
        if (onActivity)
            onActivity();
    }

  private:
    QImage  m_image;
    QString m_audioTitle;
    Center  m_center = Center::None;
    QPoint  m_pressPos;
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
        if (progress < 0) {
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
        m_mode   = Mode::Message;
        m_glyph  = glyph;
        m_title  = title;
        m_detail = detail;
        m_error  = error;
        show();
        update();
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
        if (m_progress < 0) {
            p.drawArc(ring, (90 - m_angle) * 16, -100 * 16);
        } else {
            const double value = qBound(0.0, m_progress, 1.0);
            p.drawArc(ring, 90 * 16, -qMax(1, qRound(value * 360 * 16)));
            QFont font = this->font();
            font.setBold(true);
            font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.85));
            p.setFont(font);
            p.drawText(ring, Qt::AlignCenter, QStringLiteral("%1%").arg(qRound(value * 100)));
        }

        if (!m_title.isEmpty())
            paintPill(p, c.y() + kRing + 20, m_title);
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

    void paintMessage(QPainter& p)
    {
        const int panelWidth = qMin(440, width() - 48);
        if (panelWidth < 80)
            return;
        const int textWidth = panelWidth - 40;

        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.1);
        const QFontMetrics titleFm(titleFont);
        const QFontMetrics detailFm(font());
        const QRect        titleRect  = titleFm.boundingRect(QRect(0, 0, textWidth, 1000), Qt::AlignHCenter | Qt::TextWordWrap, m_title);
        const QRect        detailRect = m_detail.isEmpty() ? QRect() : detailFm.boundingRect(QRect(0, 0, textWidth, 1000), Qt::AlignHCenter | Qt::TextWordWrap, m_detail);

        const int    glyphSize = 44;
        const int    height    = 22 + glyphSize + 14 + titleRect.height() + (m_detail.isEmpty() ? 0 : 6 + detailRect.height()) + 22;
        const QRectF panel((width() - panelWidth) / 2.0, (this->height() - height) / 2.0, panelWidth, height);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(17, 18, 20, 215));
        p.drawRoundedRect(panel, 10, 10);

        qreal y = panel.top() + 22;
        drawGlyph(p, m_glyph, QRectF(panel.center().x() - glyphSize / 2.0, y, glyphSize, glyphSize), m_error ? QColor(0xf2, 0x3f, 0x43) : QColor(0xb5, 0xba, 0xc1));
        y += glyphSize + 14;

        p.setFont(titleFont);
        p.setPen(QColor(0xf2, 0xf3, 0xf5));
        p.drawText(QRectF(panel.left() + 20, y, textWidth, titleRect.height()), Qt::AlignHCenter | Qt::TextWordWrap, m_title);
        y += titleRect.height() + 6;

        if (!m_detail.isEmpty()) {
            p.setFont(font());
            p.setPen(QColor(0xb5, 0xba, 0xc1));
            p.drawText(QRectF(panel.left() + 20, y, textWidth, detailRect.height()), Qt::AlignHCenter | Qt::TextWordWrap, m_detail);
        }
    }

    Mode        m_mode         = Mode::None;
    double      m_progress     = -1.0;
    qreal       m_promptOffset = 0;
    Glyph       m_glyph    = Glyph::Info;
    QString     m_title;
    QString     m_detail;
    bool        m_error = false;
    int         m_angle = 0;
    QBasicTimer m_spin;
};

// Bottom player bar that floats over the video with a dark gradient behind it.
class ControlBar : public QWidget
{
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter        p(this);
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(0.0, QColor(0, 0, 0, 0));
        gradient.setColorAt(0.45, QColor(0, 0, 0, 120));
        gradient.setColorAt(1.0, QColor(0, 0, 0, 210));
        p.fillRect(rect(), gradient);
    }
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
}

void ImageCanvas::setImage(const QImage& image, bool resetView)
{
    const QSize oldSize = m_image.size();
    m_image             = image;
    if (resetView || oldSize.isEmpty() || image.isNull()) {
        setFit();
        return;
    }
    if (m_fit) {
        m_zoom = fitZoom();
        emit zoomChanged(m_zoom);
    } else if (oldSize.width() != image.width() && image.width() > 0) {
        // Same picture at a different resolution (placeholder -> full image): keep its size on screen.
        m_zoom = qBound(0.05, m_zoom * oldSize.width() / image.width(), 16.0);
        emit zoomChanged(m_zoom);
    }
    clampOffset();
    update();
}

qreal ImageCanvas::fitZoom() const
{
    if (m_image.isNull())
        return 1.0;
    const qreal zx = (width() - 24) / static_cast<qreal>(m_image.width());
    const qreal zy = (height() - 24) / static_cast<qreal>(m_image.height());
    return qMax(0.01, qMin(1.0, qMin(zx, zy))); // never upscale when fitting
}

void ImageCanvas::setFit()
{
    m_fit    = true;
    m_zoom   = fitZoom();
    m_offset = {};
    clampOffset();
    update();
    emit zoomChanged(m_zoom);
}

void ImageCanvas::setActualSize()
{
    m_fit  = false;
    m_zoom = 1.0;
    clampOffset();
    update();
    emit zoomChanged(m_zoom);
}

qreal ImageCanvas::zoom() const
{
    return m_zoom;
}

void ImageCanvas::clampOffset()
{
    const QSizeF scaled = QSizeF(m_image.size()) * m_zoom;
    const qreal  maxX   = qMax(0.0, (scaled.width() - width()) / 2.0);
    const qreal  maxY   = qMax(0.0, (scaled.height() - height()) / 2.0);
    m_offset.setX(qBound(-maxX, m_offset.x(), maxX));
    m_offset.setY(qBound(-maxY, m_offset.y(), maxY));
    if (!m_dragging)
        setCursor(canPan(m_image, m_zoom, size()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void ImageCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1e, 0x1f, 0x22));
    if (m_image.isNull())
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 4.0);
    const QSizeF  scaled = QSizeF(m_image.size()) * m_zoom;
    const QPointF center = QPointF(width() / 2.0, height() / 2.0) + m_offset;
    p.drawImage(QRectF(center - QPointF(scaled.width() / 2.0, scaled.height() / 2.0), scaled), m_image);
}

void ImageCanvas::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (m_image.isNull() || delta == 0) {
        event->ignore();
        return;
    }
    // Proportional to the wheel delta so touchpads zoom smoothly; one notch = ~20%.
    const qreal newZoom = qBound(0.05, m_zoom * std::pow(1.0015, delta), 16.0);

    // Zoom around the cursor.
    const QPointF cursor = event->position() - QPointF(width() / 2.0, height() / 2.0);
    m_offset             = cursor - (cursor - m_offset) * (newZoom / m_zoom);
    m_zoom               = newZoom;
    m_fit                = false;
    clampOffset();
    update();
    emit zoomChanged(m_zoom);
}

void ImageCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging   = true;
    m_moved      = false;
    m_dragStart  = event->pos();
    m_dragOffset = m_offset;
    if (canPan(m_image, m_zoom, size()))
        setCursor(Qt::ClosedHandCursor);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    if (!m_moved && (event->pos() - m_dragStart).manhattanLength() < QApplication::startDragDistance())
        return;
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
    if (m_fit)
        setActualSize();
    else
        setFit();
}

void ImageCanvas::resizeEvent(QResizeEvent*)
{
    if (m_fit) {
        m_zoom = fitZoom();
        emit zoomChanged(m_zoom);
    }
    clampOffset();
}

// ============================================================================================
// MediaViewer
// ============================================================================================

struct MediaViewer::Private : public QObject {
    enum class Content { None, Image, Animation, Video, Audio, File };

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

    void applyStill(bool reset);
    void startLoading();
    void loadImage(const QString& path);
    void onDecoded(quint64 forGeneration, const Decoded& result);
    void startAnimation(const QString& path);
    void openPlayer(const QString& path);
    void onPlayerLoaded();
    void onEntryChanged(const QString& changedKey);
    void primaryAction();

    void updateTopBar();
    void elideName();
    void updateOverlay();
    void updateButtons();
    void updatePlaybackUi();
    void updateTime(qint64 positionMs);
    void layoutStage();
    void updateFrameSize();
    void showControls();
    void autoHideControls();
    void onWindowStateChanged();

    void togglePlay();
    void toggleGifPause();
    void seekTo(qint64 ms);
    void seekBy(qint64 deltaMs);
    void setVolume(int percent, bool persist);
    void setMuted(bool on, bool notify);
    void toggleMute();
    void toggleFullscreen();
    void copyCurrent();
    void flash(const QString& text);
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

    QThreadPool pool; // image decoding; waited for in the destructor
    QTimer*     downloadTimer = nullptr;
    QTimer*     hideTimer     = nullptr;
    QTimer*     flashTimer    = nullptr;

    QString        displayName; // displayFileName() of the shown item (nameLabel shows it elided)
    QWidget*       topBar       = nullptr;
    QLabel*        nameLabel    = nullptr;
    QLabel*        metaLabel    = nullptr;
    QLabel*        counterLabel = nullptr;
    QWidget*       stage        = nullptr;
    ImageCanvas*   canvas       = nullptr;
    VideoSurface*  surface      = nullptr;
    StatusOverlay* overlay      = nullptr;
    GlyphButton*   prevButton   = nullptr;
    GlyphButton*   nextButton   = nullptr;
    GlyphButton*   exitButton   = nullptr;
    QLabel*        flashLabel   = nullptr;

    ControlBar*  controls         = nullptr;
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

    flashTimer = new QTimer(this);
    flashTimer->setSingleShot(true);
    flashTimer->setInterval(1600);
}

MediaViewer::Private::~Private()
{
    pool.clear();
    pool.waitForDone();
}

bool MediaViewer::Private::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == stage && event->type() == QEvent::Resize)
        layoutStage();
    else if (watched == nameLabel && event->type() == QEvent::Resize)
        elideName();
    else if (watched == q && event->type() == QEvent::WindowStateChange)
        onWindowStateChanged();
    return QObject::eventFilter(watched, event);
}

void MediaViewer::Private::buildUi()
{
    auto textButton = [](const QString& text, QWidget* parent) {
        auto* button = new QPushButton(text, parent);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    // ---- top bar: name, details, gallery position --------------------------------------------
    topBar = new QWidget(q);
    topBar->setObjectName(QStringLiteral("tsmediaTopBar"));
    nameLabel = new QLabel(topBar);
    nameLabel->setObjectName(QStringLiteral("tsmediaTitle"));
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLabel->installEventFilter(this);
    metaLabel = new QLabel(topBar);
    metaLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    counterLabel = new QLabel(topBar);
    counterLabel->setObjectName(QStringLiteral("tsmediaCounter"));
    // File names come from chat links (anyone can post one): never let a name be read as rich text.
    nameLabel->setTextFormat(Qt::PlainText);
    metaLabel->setTextFormat(Qt::PlainText);
    counterLabel->setTextFormat(Qt::PlainText);
    // Left-aligned like the rest of the window, also when a name is in a right-to-left script.
    const Qt::Alignment start = Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter;
    nameLabel->setAlignment(start);
    metaLabel->setAlignment(start);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(nameLabel);
    titles->addWidget(metaLabel);
    auto* top = new QHBoxLayout(topBar);
    top->setContentsMargins(16, 10, 16, 10);
    top->setSpacing(12);
    top->addLayout(titles, 1);
    top->addWidget(counterLabel, 0, Qt::AlignVCenter);

    // ---- stage: picture / video, overlays -----------------------------------------------------
    stage = new QWidget(q);
    stage->setMinimumSize(320, 220);
    stage->installEventFilter(this);

    canvas  = new ImageCanvas(stage);
    surface = new VideoSurface(stage);
    surface->hide();
    overlay = new StatusOverlay(stage);

    controls = new ControlBar(stage);
    controls->hide();
    playButton = new GlyphButton(Glyph::Play, 34, false, controls);
    timeLabel  = new QLabel(controls);
    timeLabel->setObjectName(QStringLiteral("tsmediaTime"));
    seekSlider = new JumpSlider(false, controls);
    seekSlider->setRange(0, 0);
    muteButton   = new GlyphButton(Glyph::VolumeHigh, 34, false, controls);
    volumeSlider = new JumpSlider(true, controls);
    volumeSlider->setRange(0, 100);
    volumeSlider->setSingleStep(5);
    volumeSlider->setPageStep(10);
    volumeSlider->setFixedWidth(84);
    volumeSlider->setValue(volume);
    volumeSlider->setToolTip(i18n::t("Volume (↑ / ↓)"));
    loopButton = new GlyphButton(Glyph::Loop, 34, false, controls);
    loopButton->setCheckable(true);
    loopButton->setChecked(loop);
    loopButton->setToolTip(i18n::t("Loop"));
    fullscreenButton = new GlyphButton(Glyph::FullscreenEnter, 34, false, controls);
    fullscreenButton->setToolTip(i18n::t("Full screen (F)"));

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
    prevButton->setToolTip(i18n::t("Previous (←)"));
    nextButton = new GlyphButton(Glyph::ChevronRight, 44, true, stage);
    nextButton->setToolTip(i18n::t("Next (→)"));
    exitButton = new GlyphButton(Glyph::FullscreenExit, 40, true, stage);
    exitButton->setToolTip(i18n::t("Exit full screen (Esc)"));
    exitButton->hide();

    flashLabel = new QLabel(stage);
    flashLabel->setObjectName(QStringLiteral("tsmediaFlash"));
    flashLabel->setTextFormat(Qt::PlainText);
    flashLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    flashLabel->hide();
    connect(flashTimer, &QTimer::timeout, flashLabel, &QWidget::hide);

    // ---- action bar ---------------------------------------------------------------------------
    actionBar = new QWidget(q);
    actionBar->setObjectName(QStringLiteral("tsmediaActionBar"));
    fitButton = textButton(i18n::t("Fit"), actionBar);
    fitButton->setToolTip(i18n::t("Fit to window (0)"));
    actualButton = textButton(QStringLiteral("100%"), actionBar);
    actualButton->setToolTip(i18n::t("Actual size (1)"));
    zoomLabel = new QLabel(actionBar);
    zoomLabel->setMinimumWidth(48);
    actionButton = textButton(QString(), actionBar);
    actionButton->setObjectName(QStringLiteral("primary"));
    actionButton->hide();
    copyButton   = textButton(i18n::t("Copy image"), actionBar);
    saveButton   = textButton(i18n::t("Save as…"), actionBar);
    saveButton->setToolTip(i18n::t("Save a copy (Ctrl+S)"));
    folderButton = textButton(i18n::t("Show in folder"), actionBar);
    openButton   = textButton(i18n::t("Open with default app"), actionBar);
    openButton->setObjectName(QStringLiteral("primary"));

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
    connect(canvas, &ImageCanvas::zoomChanged, this, [this](qreal z) { zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });
    connect(canvas, &ImageCanvas::clicked, this, [this] {
        if (content == Content::Animation && movie)
            toggleGifPause();
    });
    surface->onClick       = [this] { togglePlay(); };
    surface->onDoubleClick = [this] {
        togglePlay(); // undo the toggle of the first click of the double click
        toggleFullscreen();
    };
    surface->onActivity = [this] { showControls(); };

    connect(prevButton, &QAbstractButton::clicked, this, [this] { step(-1); });
    connect(nextButton, &QAbstractButton::clicked, this, [this] { step(1); });
    connect(exitButton, &QAbstractButton::clicked, this, [this] { toggleFullscreen(); });
    connect(playButton, &QAbstractButton::clicked, this, [this] { togglePlay(); });
    connect(muteButton, &QAbstractButton::clicked, this, [this] { toggleMute(); });
    connect(fullscreenButton, &QAbstractButton::clicked, this, [this] { toggleFullscreen(); });
    connect(loopButton, &QAbstractButton::toggled, this, [this](bool on) {
        loop = on;
        if (player)
            player->setLoop(on);
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

    connect(fitButton, &QPushButton::clicked, canvas, &ImageCanvas::setFit);
    connect(actualButton, &QPushButton::clicked, canvas, &ImageCanvas::setActualSize);
    connect(actionButton, &QPushButton::clicked, this, [this] { primaryAction(); });
    connect(copyButton, &QPushButton::clicked, this, [this] { copyCurrent(); });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        if (core)
            core->saveAs(currentKey(), q);
    });
    connect(folderButton, &QPushButton::clicked, this, [this] {
        if (core)
            core->revealInFolder(currentKey());
    });
    connect(openButton, &QPushButton::clicked, this, [this] {
        if (player && player->isPlaying())
            player->pause();
        if (core)
            core->openExternally(currentKey());
    });

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
    if (target >= 0 && target < keys.size())
        showItem(target);
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
        surface->setAudioTitle(displayFileName(e->link.fileName));
    applyStill(true);

    {
        const QSignalBlocker blocker(seekSlider);
        seekSlider->setRange(0, 0);
        seekSlider->setValue(0);
    }
    copyButton->setText(content == Content::Video ? i18n::t("Copy frame") : i18n::t("Copy image"));
    copyButton->setToolTip(i18n::t("Copy to clipboard (Ctrl+C)"));

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
        errorTitle   = content == Content::Audio ? i18n::t("This file can't be played here")
                                                 : i18n::t("This video can't be played here");
        errorDetail  = i18n::t("Try opening it with your default app.");
        if (!error.isEmpty())
            errorDetail += QLatin1Char('\n') + error;
        updatePlaybackUi();
        updateOverlay();
        updateButtons();
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
// as Core's automatic downloads (Settings::videoAutoDownloadMB, 0 = "Never (when I press play)"; the
// setting is off without inline previews). Files Core stopped because their real size exceeded the
// limit (tooLargeForAuto) and links of unknown size are never fetched unasked: they wait for Play.
bool MediaViewer::Private::fetchesAutomatically() const
{
    const MediaEntry* e = entry();
    if (!e || content == Content::None || content == Content::File)
        return false;
    if (isPicture() || requested.contains(currentKey()))
        return true;
    const Settings& s     = Settings::instance();
    const quint64   limit = static_cast<quint64>(qMax(0, s.videoAutoDownloadMB)) * 1024 * 1024;
    return s.inlinePreviews && limit > 0 && e->link.size > 0 && e->link.size <= limit && !e->tooLargeForAuto;
}

// A video / audio that is not downloaded and is not fetched automatically: it shows its poster with a
// play button that downloads it (and plays it once it is there).
bool MediaViewer::Private::waitingForPlay() const
{
    const MediaEntry* e = entry();
    return isPlayable() && e && e->state == MediaState::Idle && !fetchesAutomatically();
}

// ---- presentation ---------------------------------------------------------------------------

void MediaViewer::Private::updateTopBar()
{
    const MediaEntry* e = entry();
    if (!e)
        return;
    // The name comes from a chat link: shown without control / bidi override characters, and the
    // tooltip (which Qt would read as rich text if it looked like HTML) is escaped.
    const QString name = displayFileName(e->link.fileName);
    displayName        = name;
    q->setWindowTitle(QStringLiteral("%1 — " TSMEDIA_NAME).arg(name));
    nameLabel->setToolTip(QStringLiteral("<p style='white-space:pre'>%1</p>").arg(name.toHtmlEscaped()));
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

    counterLabel->setText(i18n::t("%1 / %2").arg(index + 1).arg(keys.size()));
    counterLabel->setVisible(keys.size() > 1);
}

void MediaViewer::Private::elideName()
{
    nameLabel->setText(nameLabel->fontMetrics().elidedText(displayName, Qt::ElideMiddle, qMax(40, nameLabel->width())));
}

void MediaViewer::Private::updateOverlay()
{
    const MediaEntry* e = entry();
    if (!e) {
        overlay->clear();
        return;
    }
    if (!errorTitle.isEmpty()) {
        overlay->showMessage(Glyph::Error, errorTitle, errorDetail, true);
        return;
    }
    if (tooLarge) {
        overlay->showMessage(Glyph::Info, i18n::t("This image is too large to show here"),
                             i18n::t("%1 pixels. Open it with your default app instead.")
                                 .arg(QStringLiteral("%1 × %2").arg(tooLargeSize.width()).arg(tooLargeSize.height())),
                             false);
        return;
    }
    const bool showing = loaded || (player && player->isLoaded() && !playerFailed);
    if (showing) {
        if (content == Content::Animation && movie && movie->state() == QMovie::Paused)
            overlay->showPaused();
        else
            overlay->clear();
        return;
    }
    if (e->state == MediaState::Failed) {
        const QString detail = e->errorText.isEmpty() ? i18n::t("Something went wrong while downloading this file.") : e->errorText;
        overlay->showMessage(Glyph::Error, i18n::t("Download failed"), detail, true);
        return;
    }

    auto downloadText = [e] {
        if (e->link.size == 0)
            return i18n::t("Downloading…");
        const quint64 done = static_cast<quint64>(qBound(0.0, e->progress, 1.0) * static_cast<double>(e->link.size));
        return i18n::t("Downloading… %1 of %2").arg(formatSize(done), formatSize(e->link.size));
    };

    if (content == Content::File) {
        if (e->state == MediaState::Downloading)
            overlay->showProgress(e->progress, downloadText());
        else if (e->state == MediaState::Queued)
            overlay->showProgress(-1, i18n::t("Waiting to download…"));
        else
            overlay->showMessage(Glyph::File, displayFileName(e->link.fileName),
                                 (e->link.size > 0 ? formatSize(e->link.size) + QStringLiteral("  ·  ") : QString()) + i18n::t("No preview available"),
                                 false);
        return;
    }

    switch (e->state) {
    case MediaState::Downloading:
        overlay->showProgress(e->progress, downloadText());
        return;
    case MediaState::Queued:
        overlay->showProgress(-1, i18n::t("Waiting to download…"));
        return;
    case MediaState::Idle:
        if (waitingForPlay()) {
            const QString text = e->link.size > 0 ? i18n::t("Press play to download (%1)").arg(formatSize(e->link.size))
                                                  : i18n::t("Press play to download");
            // Under the play button; for audio under the file name the surface draws below its icon.
            overlay->showPrompt(text, content == Content::Audio ? 100 : kCenterButton + 14);
        } else {
            overlay->showProgress(-1, i18n::t("Loading…"));
        }
        return;
    case MediaState::Ready:
    case MediaState::Failed:
        break;
    }

    if (isPlayable() && player && !player->isLoaded() && !playerFailed) {
        overlay->showProgress(-1, QString());
        return;
    }
    if (isPicture() && started && stillSource < MediaStill::Preview) {
        overlay->showProgress(-1, QString());
        return;
    }
    overlay->clear();
}

void MediaViewer::Private::updateButtons()
{
    const MediaEntry* e     = entry();
    const bool        ready = e && e->state == MediaState::Ready;

    fitButton->setVisible(isPicture());
    actualButton->setVisible(isPicture());
    zoomLabel->setVisible(isPicture() && loaded);
    fitButton->setEnabled(loaded);
    actualButton->setEnabled(loaded);

    const bool canCopy = (content == Content::Image && !fullImage.isNull()) || (content == Content::Animation && loaded && movie) || (content == Content::Video && hasFrame);
    copyButton->setVisible(isPicture() || content == Content::Video);
    copyButton->setEnabled(canCopy);
    saveButton->setEnabled(ready);
    folderButton->setEnabled(ready);
    openButton->setEnabled(ready);

    if (e && e->state == MediaState::Failed && e->error != MediaError::NotFound) {
        actionButton->setText(i18n::t("Retry"));
        actionButton->show();
    } else if (e && content == Content::File && e->state == MediaState::Idle) {
        actionButton->setText(i18n::t("Download"));
        actionButton->show();
    } else if (waitingForPlay()) {
        actionButton->setText(i18n::t("Download and play"));
        actionButton->show();
    } else {
        actionButton->hide();
    }
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
    playButton->setToolTip(playing    ? i18n::t("Pause (Space)")
                           : ended    ? i18n::t("Replay (Space)")
                           : awaiting ? i18n::t("Download and play (Space)")
                                      : i18n::t("Play (Space)"));

    const bool silent = muted || volume == 0;
    muteButton->setGlyph(silent ? Glyph::VolumeMuted : volume < 50 ? Glyph::VolumeLow : Glyph::VolumeHigh);
    muteButton->setToolTip(silent ? i18n::t("Unmute (M)") : i18n::t("Mute (M)"));
    {
        const QSignalBlocker blocker(volumeSlider);
        volumeSlider->setValue(muted ? 0 : volume);
    }

    surface->setCenter(ready && !playing ? (ended ? VideoSurface::Center::Replay : VideoSurface::Center::Play)
                       : awaiting        ? VideoSurface::Center::Play
                                         : VideoSurface::Center::None);
    if (ready && !seekSlider->isSliderDown() && pendingSeekMs < 0)
        updateTime(player->position());
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

    const int nav    = 44;
    const int margin = 16;
    const int navY   = (r.height() - nav) / 2;
    prevButton->setGeometry(margin, navY, nav, nav);
    nextButton->setGeometry(r.width() - margin - nav, navY, nav, nav);
    exitButton->setGeometry(r.width() - margin - exitButton->width(), margin, exitButton->width(), exitButton->height());

    const int barHeight = controls->sizeHint().height();
    controls->setGeometry(0, r.height() - barHeight, r.width(), barHeight);

    flashLabel->adjustSize();
    flashLabel->move((r.width() - flashLabel->width()) / 2, 20);
    updateFrameSize();
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
    controls->setVisible(isPlayable());
    const bool gallery = keys.size() > 1;
    prevButton->setVisible(gallery && index > 0);
    nextButton->setVisible(gallery && index < keys.size() - 1);
    exitButton->setVisible(q->isFullScreen());
    surface->setCursor(Qt::ArrowCursor);
    if (isPlayable() && player && player->isPlaying())
        hideTimer->start();
    else
        hideTimer->stop();
}

void MediaViewer::Private::autoHideControls()
{
    if (!player || !player->isPlaying())
        return;
    const bool interacting = controls->underMouse() || prevButton->underMouse() || nextButton->underMouse() || exitButton->underMouse() || seekSlider->isSliderDown() || volumeSlider->isSliderDown();
    if (interacting) {
        hideTimer->start();
        return;
    }
    controls->hide();
    prevButton->hide();
    nextButton->hide();
    exitButton->hide();
    surface->setCursor(Qt::BlankCursor);
}

void MediaViewer::Private::onWindowStateChanged()
{
    const bool fullscreen = q->isFullScreen();
    topBar->setVisible(!fullscreen);
    actionBar->setVisible(!fullscreen);
    fullscreenButton->setGlyph(fullscreen ? Glyph::FullscreenExit : Glyph::FullscreenEnter);
    fullscreenButton->setToolTip(fullscreen ? i18n::t("Exit full screen (F)") : i18n::t("Full screen (F)"));
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

void MediaViewer::Private::copyCurrent()
{
    QImage image;
    if (content == Content::Image)
        image = fullImage;
    else if (content == Content::Animation && movie)
        image = movie->currentImage();
    else if (content == Content::Video && player && hasFrame)
        image = player->currentFrame().convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return;
    QApplication::clipboard()->setImage(image);
    flash(i18n::t("Copied to clipboard"));
}

void MediaViewer::Private::flash(const QString& text)
{
    flashLabel->setText(text);
    flashLabel->adjustSize();
    flashLabel->move((stage->width() - flashLabel->width()) / 2, 20);
    flashLabel->show();
    flashLabel->raise();
    flashTimer->start();
}

bool MediaViewer::Private::handleKey(QKeyEvent* event)
{
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl  = event->modifiers().testFlag(Qt::ControlModifier);

    // With a non-Latin keyboard layout the letter keys produce other characters;
    // fall back to the physical key so F, M, K, 0, 1, Ctrl+C and Ctrl+S keep working.
    int           key = event->key();
    const quint32 vk  = event->nativeVirtualKey();
    if (key > 0x7e && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')))
        key = static_cast<int>(vk);

    switch (key) {
    case Qt::Key_Escape:
        if (q->isFullScreen())
            toggleFullscreen();
        else
            q->close();
        return true;
    case Qt::Key_Space:
    case Qt::Key_K:
        if (isPlayable())
            togglePlay();
        else if (content == Content::Animation)
            toggleGifPause();
        return true;
    case Qt::Key_Left:
        if (shift && isPlayable())
            seekBy(-5000);
        else
            step(-1);
        return true;
    case Qt::Key_Right:
        if (shift && isPlayable())
            seekBy(5000);
        else
            step(1);
        return true;
    case Qt::Key_Home:
        if (isPlayable())
            seekTo(0);
        return true;
    case Qt::Key_F:
        toggleFullscreen();
        return true;
    case Qt::Key_M:
        if (isPlayable())
            toggleMute();
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (isPlayable())
            setVolume((muted ? 0 : volume) + (key == Qt::Key_Up ? 5 : -5), true);
        return true;
    case Qt::Key_0:
        if (isPicture() && loaded)
            canvas->setFit();
        return true;
    case Qt::Key_1:
        if (isPicture() && loaded)
            canvas->setActualSize();
        return true;
    case Qt::Key_C:
        if (!ctrl)
            return false;
        copyCurrent();
        return true;
    case Qt::Key_S:
        if (!ctrl)
            return false;
        if (core && entry() && entry()->state == MediaState::Ready)
            core->saveAs(currentKey(), q);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return true; // no default button in this window
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
    setObjectName(QStringLiteral("tsmediaMediaViewer"));
    setLayoutDirection(Qt::LeftToRight);
    setFocusPolicy(Qt::StrongFocus);
    // fromLatin1, never QStringLiteral: a window with a parent shares the parent's QStyleSheetStyle when
    // TeamSpeak uses style sheets, and its parser keeps the last text it parsed until TeamSpeak destroys
    // it at exit, after this DLL is unloaded (~QString on literal data in the unloaded DLL = crash).
    setStyleSheet(QString::fromLatin1(
        "#tsmediaMediaViewer { background: #1e1f22; }"
        "#tsmediaMediaViewer QLabel { color: #b5bac1; background: transparent; }"
        "#tsmediaMediaViewer QLabel#tsmediaTitle { color: #f2f3f5; font-weight: bold; }"
        "#tsmediaMediaViewer QLabel#tsmediaCounter { color: #dbdee1; background: rgba(255,255,255,20); border-radius: 10px; padding: 3px 10px; }"
        "#tsmediaMediaViewer QLabel#tsmediaTime { color: #f2f3f5; }"
        "#tsmediaMediaViewer QLabel#tsmediaFlash { color: #f2f3f5; background: rgba(17,18,20,225); border-radius: 6px; padding: 7px 14px; }"
        "#tsmediaMediaViewer QPushButton { color: #f2f3f5; background: #4e5058; border: none; border-radius: 4px; padding: 6px 12px; }"
        "#tsmediaMediaViewer QPushButton:hover { background: #6d6f78; }"
        "#tsmediaMediaViewer QPushButton:disabled { color: #80848e; background: #3a3c42; }"
        "#tsmediaMediaViewer QPushButton#primary { background: #5865f2; }"
        "#tsmediaMediaViewer QPushButton#primary:hover { background: #4752c4; }"
        "#tsmediaMediaViewer QPushButton#primary:disabled { color: #a3a6aa; background: #3c4270; }"
        "#tsmediaMediaViewer QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,70); border-radius: 2px; }"
        "#tsmediaMediaViewer QSlider::sub-page:horizontal { background: #5865f2; border-radius: 2px; }"
        "#tsmediaMediaViewer QSlider::add-page:horizontal { background: rgba(255,255,255,70); border-radius: 2px; }"
        "#tsmediaMediaViewer QSlider::handle:horizontal { background: #ffffff; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
        "#tsmediaMediaViewer QSlider::sub-page:horizontal:disabled { background: rgba(255,255,255,40); }"
        "#tsmediaMediaViewer QSlider::handle:horizontal:disabled { background: rgba(255,255,255,90); }"
        "QToolTip { color: #f2f3f5; background: #111214; border: 1px solid #3f4147; padding: 4px 6px; }"));

    d->buildUi();
    d->setGallery(keys, index);

    // Size the window around the first item, within 85% of the screen.
    const QScreen*    screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize       avail  = screen ? screen->availableGeometry().size() * 0.85 : QSize(1280, 800);
    const MediaEntry* e      = d->entry();
    const bool        sized  = e && e->link.width > 0 && e->link.height > 0;
    QSize             media  = sized ? QSize(e->link.width, e->link.height) : QSize(960, 600);
    const QSize       chrome(24, 140);
    if (sized && (media.width() + chrome.width() > avail.width() || media.height() + chrome.height() > avail.height()))
        media = media.scaled(avail - chrome, Qt::KeepAspectRatio);
    resize((media + chrome).expandedTo(QSize(720, 520)).boundedTo(avail));

    useDarkTitleBar(this);
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
