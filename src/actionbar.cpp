#include "actionbar.h"

#include <QAbstractButton>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>
#include <QVariantAnimation>

#include "i18n.h"
#include "uiutil.h"

namespace {

constexpr int kPad       = 2;
constexpr int kFadeInMs  = 80;
constexpr int kFadeOutMs = 60;
constexpr int kCopiedMs  = 1200;

class BarButton : public QAbstractButton
{
  public:
    BarButton(layoutart::Glyph glyph, const QString& text, QWidget* parent)
        : QAbstractButton(parent)
        , m_glyph(glyph)
    {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setToolTip(text);
        setAccessibleName(text);
    }

    void setColors(const layoutart::Colors* colors, int icon)
    {
        m_colors = colors;
        m_icon   = icon;
        update();
    }
    void setGlyph(layoutart::Glyph glyph)
    {
        m_glyph = glyph;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!m_colors)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hot = underMouse() || isDown();
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? m_colors->barPressed : m_colors->barHoverBack);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
        }
        const QRectF box(rect().center().x() + 0.5 - m_icon / 2.0, rect().center().y() + 0.5 - m_icon / 2.0, m_icon, m_icon);
        layoutart::drawGlyph(p, m_glyph, box, hot ? m_colors->barHoverIcon : m_colors->barIcon);
    }

  private:
    layoutart::Glyph         m_glyph;
    const layoutart::Colors* m_colors = nullptr;
    int                      m_icon   = 18;
};

} // namespace

ActionBar::ActionBar(QWidget* viewport)
    : QWidget(viewport)
{
    // Swept at plugin shutdown like our other widgets inside TeamSpeak's. fromLatin1 / i18n::t: heap strings.
    setObjectName(QString::fromLatin1("tsmediaActionBar"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_NoSystemBackground);
    setLayoutDirection(Qt::LeftToRight);
    setAccessibleName(i18n::t("Message actions"));
    const struct {
        layoutart::Glyph glyph;
        const char*      text;
    } items[] = {{layoutart::Glyph::React, "Add reaction"}, {layoutart::Glyph::Reply, "Reply"}, {layoutart::Glyph::Copy, "Copy text"}, {layoutart::Glyph::More, "More"}};
    for (int i = 0; i < 4; ++i) {
        auto* button = new BarButton(items[i].glyph, i18n::t(items[i].text), this);
        button->setObjectName(QString::fromLatin1("tsmediaActionBarButton%1").arg(i));
        connect(button, &QAbstractButton::clicked, this, [this, i] { emit triggered(i); });
        m_buttons.append(button);
    }
    m_copied = new QTimer(this); // owned: stops with the bar
    m_copied->setSingleShot(true);
    m_copied->setInterval(kCopiedMs);
    connect(m_copied, &QTimer::timeout, this, [this] {
        static_cast<BarButton*>(m_buttons.at(Copy))->setGlyph(layoutart::Glyph::Copy);
        m_buttons.at(Copy)->setToolTip(i18n::t("Copy text"));
    });
    m_fade = new QVariantAnimation(this);
    connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (m_effect)
            m_effect->setOpacity(value.toReal());
    });
    connect(m_fade, &QVariantAnimation::finished, this, [this] {
        if (m_hiding) {
            hide();
            m_hiding = false;
        }
        setGraphicsEffect(nullptr); // deletes it
        m_effect = nullptr;
    });
    hide();
}

ActionBar::~ActionBar() = default;

QAbstractButton* ActionBar::button(int action) const
{
    return m_buttons.value(action, nullptr);
}

void ActionBar::setLook(const layoutart::Colors& colors, bool compact)
{
    m_colors  = colors;
    m_compact = compact;
    for (QAbstractButton* b : qAsConst(m_buttons))
        static_cast<BarButton*>(b)->setColors(&m_colors, compact ? 16 : 18);
    relayout();
    update();
}

void ActionBar::setReactVisible(bool visible)
{
    if (m_buttons.at(React)->isVisibleTo(this) == visible)
        return;
    m_buttons.at(React)->setVisible(visible);
    relayout();
}

void ActionBar::relayout()
{
    const int size = m_compact ? 24 : 28;
    int       x    = kShadow + 1 + kPad;
    for (QAbstractButton* b : qAsConst(m_buttons)) {
        if (b->isHidden())
            continue;
        b->setGeometry(x, kShadow + 1 + kPad, size, size);
        x += size;
    }
    resize(x + kPad + 1 + kShadow, size + 2 * (kPad + 1) + 2 * kShadow);
}

QRect ActionBar::barRect() const
{
    return rect().adjusted(kShadow, kShadow, -kShadow, -kShadow);
}

void ActionBar::fadeTo(qreal target, int ms)
{
    m_fade->stop();
    if (!m_effect) {
        m_effect = new QGraphicsOpacityEffect(this);
        m_effect->setOpacity(target > 0.5 ? 0.0 : 1.0);
        setGraphicsEffect(m_effect);
    }
    m_fade->setStartValue(m_effect->opacity());
    m_fade->setEndValue(target);
    m_fade->setDuration(ms);
    m_fade->setEasingCurve(target > 0.5 ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    m_fade->start();
}

void ActionBar::showBar(bool animate)
{
    const bool was = isVisible() && !m_hiding;
    m_hiding       = false;
    show();
    raise();
    if (was)
        return;
    if (animate && ui::animationsEnabled()) {
        fadeTo(1.0, kFadeInMs);
    } else {
        m_fade->stop();
        setGraphicsEffect(nullptr);
        m_effect = nullptr;
    }
}

void ActionBar::hideBar(bool animate)
{
    if (!isVisible() || m_hiding)
        return;
    QToolTip::hideText();
    if (animate && ui::animationsEnabled()) {
        m_hiding = true;
        fadeTo(0.0, kFadeOutMs);
        return;
    }
    m_fade->stop();
    setGraphicsEffect(nullptr);
    m_effect = nullptr;
    hide();
}

void ActionBar::flashCopied()
{
    auto* copy = static_cast<BarButton*>(m_buttons.at(Copy));
    copy->setGlyph(layoutart::Glyph::Check);
    copy->setToolTip(i18n::t("Copied"));
    QToolTip::showText(copy->mapToGlobal(QPoint(copy->width() / 2, -4)), i18n::t("Copied"), copy, QRect(), kCopiedMs);
    m_copied->start();
}

void ActionBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(barRect());
    // The shadow (0 1 3 px): a few widening, fainter rounded rects, cheaper than a graphics effect.
    for (int i = 3; i >= 1; --i) {
        QColor c = m_colors.barShadow;
        c.setAlphaF(m_colors.barShadow.alphaF() * (1.0 - i / 4.0) * 0.6);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(box.adjusted(-i, -i + 1, i, i + 1), 6 + i, 6 + i);
    }
    p.setPen(QPen(m_colors.barBorder, 1.0));
    p.setBrush(m_colors.barBack);
    p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
}
