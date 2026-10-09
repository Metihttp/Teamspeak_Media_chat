#include "spoilercover.h"

#include <QApplication>
#include <QEasingCurve>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include "i18n.h"
#include "spoiler.h"
#include "uiutil.h"

namespace {

constexpr qreal kEyeSize   = 40.0;
constexpr qreal kEyeGap    = 14.0; // eye to button
constexpr qreal kHintGap   = 10.0; // button to hint
constexpr int   kFadeTick  = 16;

} // namespace

SpoilerCover::SpoilerCover(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QString::fromLatin1("tsmediaSpoilerCover"));
    setCursor(Qt::PointingHandCursor);

    // The viewer's style sheet draws QPushButton#primary in the accent colour (its main action).
    m_button = new QPushButton(i18n::t("Reveal spoiler"), this);
    m_button->setObjectName(QString::fromLatin1("primary"));
    m_button->setFocusPolicy(Qt::TabFocus); // like the viewer's buttons: a click leaves the keys on the media
    m_button->setAutoDefault(false);
    m_button->setCursor(Qt::PointingHandCursor);
    m_button->setToolTip(i18n::t("Reveal spoiler (Space)"));
    m_button->setAccessibleDescription(hintText());
    connect(m_button, &QPushButton::clicked, this, &SpoilerCover::revealRequested);

    m_fadeTimer = new QTimer(this);
    m_fadeTimer->setInterval(kFadeTick);
    connect(m_fadeTimer, &QTimer::timeout, this, [this] {
        update();
        if (m_fadeClock.isValid() && m_fadeClock.elapsed() >= qMax(spoiler::kRevealMs, QApplication::doubleClickInterval()))
            dismiss();
    });
    hide();
}

QString SpoilerCover::hintText() const
{
    return i18n::t("Click or press Space to reveal");
}

void SpoilerCover::cover(const QImage& picture, bool isBlurHash)
{
    m_fadeTimer->stop();
    m_fadeClock.invalidate();
    m_pressed  = false;
    m_picture  = picture;
    m_blurHash = isBlurHash;
    m_button->show();
    setCursor(Qt::PointingHandCursor);
    placeContent();
    show();
    update();
}

void SpoilerCover::setPicture(const QImage& picture, bool isBlurHash)
{
    if (picture.cacheKey() == m_picture.cacheKey() && isBlurHash == m_blurHash)
        return;
    m_picture  = picture;
    m_blurHash = isBlurHash;
    update();
}

void SpoilerCover::fadeOut()
{
    if (m_fadeClock.isValid())
        return;
    show(); // the viewer may have hidden it while showing the revealed item: the fade starts from the cover
    if (m_button->hasFocus() && window())
        window()->setFocus(Qt::OtherFocusReason); // the keys go back to the media
    m_button->hide();
    m_pressed = false;
    setCursor(Qt::ArrowCursor);
    m_fadeClock.start();
    m_fadeTimer->start();
    update();
}

void SpoilerCover::dismiss()
{
    m_fadeTimer->stop();
    m_fadeClock.invalidate();
    m_pressed = false;
    hide();
}

bool SpoilerCover::isCovering() const
{
    return isVisible() && !m_fadeClock.isValid();
}

qreal SpoilerCover::opacity() const
{
    if (!m_fadeClock.isValid())
        return 1.0;
    if (!ui::animationsEnabled())
        return 0.0;
    const qreal t = qBound(0.0, static_cast<qreal>(m_fadeClock.elapsed()) / spoiler::kRevealMs, 1.0);
    return 1.0 - QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t);
}

void SpoilerCover::placeContent()
{
    const QFontMetricsF fm(font());
    const QSize         button = m_button->sizeHint();
    const qreal         hintH  = qCeil(fm.height());
    const qreal         total  = kEyeSize + kEyeGap + button.height() + kHintGap + hintH;
    // Centred; the mark goes first when the stage is too low for all of it.
    const bool  withEye = height() >= total + 24.0;
    const qreal block   = withEye ? total : button.height() + kHintGap + hintH;
    qreal       y       = (height() - block) / 2.0;
    if (withEye) {
        m_eye = QRectF((width() - kEyeSize) / 2.0, y, kEyeSize, kEyeSize);
        y += kEyeSize + kEyeGap;
    } else {
        m_eye = QRectF();
    }
    m_button->setGeometry(qRound((width() - button.width()) / 2.0), qRound(y), button.width(), button.height());
    y += button.height() + kHintGap;
    m_hint = QRectF(16.0, y, qMax(0.0, width() - 32.0), hintH);
}

void SpoilerCover::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    placeContent();
}

void SpoilerCover::paintCover(QPainter& p)
{
    const QRectF bounds(rect());
    spoiler::CoverLook look;
    look.placeholder = QColor(0x2b, 0x2d, 0x31);
    look.withPill    = false; // the button says it
    spoiler::drawCover(p, bounds, m_picture, m_blurHash, look);
    p.fillRect(bounds, QColor(0, 0, 0, m_blurHash ? spoiler::kViewerBlurHashDimAlpha : spoiler::kViewerDimAlpha));

    if (!m_button->isVisible())
        return; // fading out: the picture only
    if (!m_eye.isEmpty())
        spoiler::drawEyeOffIcon(p, m_eye, QColor(255, 255, 255, 220));
    if (m_hint.width() > 0) {
        const QFontMetricsF fm(font());
        p.setFont(font());
        p.setPen(QColor(255, 255, 255, spoiler::kViewerHintAlpha));
        p.drawText(m_hint, Qt::AlignHCenter | Qt::AlignVCenter, fm.elidedText(hintText(), Qt::ElideRight, m_hint.width()));
    }
}

void SpoilerCover::paintEvent(QPaintEvent*)
{
    const qreal o = opacity();
    if (o <= 0.0)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    if (o >= 1.0) {
        paintCover(p);
        return;
    }
    // Faded as one layer (see spoiler::drawCover).
    const qreal dpr = devicePixelRatioF();
    QImage      layer(QSize(qMax(1, qCeil(width() * dpr)), qMax(1, qCeil(height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    {
        QPainter lp(&layer);
        lp.setRenderHints(p.renderHints());
        paintCover(lp);
    }
    p.setOpacity(o);
    p.drawImage(QRectF(rect()), layer);
}

void SpoilerCover::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    if (QWidget* w = window())
        w->setFocus(Qt::MouseFocusReason); // Space and the arrows keep acting on the viewer
    m_pressed = event->button() == Qt::LeftButton && isCovering();
}

void SpoilerCover::mouseReleaseEvent(QMouseEvent* event)
{
    event->accept();
    const bool click = m_pressed && event->button() == Qt::LeftButton && rect().contains(event->pos());
    m_pressed        = false;
    if (click && isCovering())
        emit revealRequested();
}

void SpoilerCover::mouseDoubleClickEvent(QMouseEvent* event)
{
    event->accept(); // the first click already revealed it
}

void SpoilerCover::wheelEvent(QWheelEvent* event)
{
    event->accept(); // nothing to zoom while covered
}
