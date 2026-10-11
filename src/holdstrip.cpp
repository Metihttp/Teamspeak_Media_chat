#include "holdstrip.h"

#include <QAccessible>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QtMath>

#include <cmath>

#include "audio/waveform.h"
#include "i18n.h"
#include "medialink.h"
#include "replyart.h"
#include "uiutil.h"

namespace {

constexpr int    kFrameMs   = 40;   // 25 frames a second, only while shown and animated
constexpr qint64 kPeriodMs  = 1200; // the dot's breath, the spinner's turn
constexpr qreal  kPad       = 12.0; // left and right
constexpr qreal  kMark      = 12.0; // the dot, the bin, the spinner
constexpr qreal  kGap       = 12.0; // between the parts
constexpr qreal  kBar       = 2.0;
constexpr qreal  kBarGap    = 2.0;
constexpr qreal  kMinWave   = 48.0; // narrower: the short hint, then no waveform
constexpr qreal  kRadius    = 6.0;  // the top corners (the reply bar's)

// The strip's colours for the input's theme; texts keep 4.5:1 on the strip, marks 3:1.
struct Colors {
    QColor surface;
    QColor line;   // the hairline along the top edge
    QColor text;
    QColor muted;
    QColor error;  // texts in the error colour
    QColor record; // the dot
    QColor live;   // the live bars
    QColor quiet;  // places not recorded yet, frozen bars
    QColor accent; // notice icons
};

Colors colorsFor(const HoldStrip::Look& look, HoldStrip::Mode mode)
{
    const replyart::Palette pal  = replyart::paletteFor(look.dark, look.base);
    const QColor            base = look.base.isValid() && look.base.alpha() == 255 ? look.base : (look.dark ? QColor(0x31, 0x33, 0x38) : QColor(Qt::white));
    const QColor            red  = look.dark ? QColor(0xf2, 0x3f, 0x43) : QColor(0xda, 0x37, 0x3c);
    Colors                  c;
    c.surface = mode == HoldStrip::Mode::Cancel ? ui::flatten(QColor(red.red(), red.green(), red.blue(), look.dark ? 46 : 28), base) : pal.surface;
    c.line    = mode == HoldStrip::Mode::Cancel ? ui::flatten(QColor(red.red(), red.green(), red.blue(), 140), base)
                                                : ui::flatten(look.dark ? QColor(255, 255, 255, 24) : QColor(0, 0, 0, 22), base);
    c.text    = replyart::readable(pal.text, c.surface, pal.text, 7.0);
    c.muted   = replyart::readable(pal.muted, c.surface, pal.muted, 4.5);
    c.error   = replyart::readable(look.dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d), c.surface, c.text, 4.5);
    c.record  = red;
    c.live    = replyart::readable(look.dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x58, 0x65, 0xf2), c.surface, c.text, 3.0);
    c.quiet   = ui::flatten(QColor(0x80, 0x84, 0x8e, 150), c.surface);
    c.accent  = replyart::readable(pal.accent, c.surface, c.text, 3.0);
    return c;
}

QFont hintFont(const QFont& input)
{
    return replyart::scaledFont(input, 0.92);
}

QFont timeFont(const QFont& input)
{
    QFont font = replyart::scaledFont(input, 1.0);
    font.setWeight(QFont::DemiBold);
    return font;
}

QString hintText(bool shortForm)
{
    return shortForm ? i18n::t("Release to send") : i18n::t("Release to send · Move away to cancel");
}

// A small bin: the recording goes away when you let go here.
void drawBin(QPainter& p, const QRectF& box, const QColor& color)
{
    QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const qreal w = box.width(), h = box.height(), x = box.left(), y = box.top();
    p.drawLine(QPointF(x + 0.08 * w, y + 0.22 * h), QPointF(x + 0.92 * w, y + 0.22 * h)); // the lid
    p.drawLine(QPointF(x + 0.38 * w, y + 0.06 * h), QPointF(x + 0.62 * w, y + 0.06 * h)); // its handle
    QPainterPath body;
    body.moveTo(x + 0.2 * w, y + 0.32 * h);
    body.lineTo(x + 0.27 * w, y + 0.94 * h);
    body.lineTo(x + 0.73 * w, y + 0.94 * h);
    body.lineTo(x + 0.8 * w, y + 0.32 * h);
    p.drawPath(body);
    p.drawLine(QPointF(x + 0.43 * w, y + 0.45 * h), QPointF(x + 0.45 * w, y + 0.8 * h));
    p.drawLine(QPointF(x + 0.57 * w, y + 0.45 * h), QPointF(x + 0.55 * w, y + 0.8 * h));
}

// The notice's icon in a circle: i (a hint), a check (sent), a cross (canceled).
void drawNoticeIcon(QPainter& p, const QRectF& box, HoldStrip::Icon icon, const QColor& color)
{
    const QPointF c = box.center();
    const qreal   r = box.width() / 2.0 - 0.8;
    p.setPen(QPen(color, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);
    QPen mark(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(mark);
    switch (icon) {
    case HoldStrip::Icon::Info:
        p.drawLine(QPointF(c.x(), c.y() - 0.05 * r), QPointF(c.x(), c.y() + 0.5 * r));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(c.x(), c.y() - 0.45 * r), 1.1, 1.1);
        break;
    case HoldStrip::Icon::Sent: {
        QPainterPath check;
        check.moveTo(c.x() - 0.45 * r, c.y() + 0.02 * r);
        check.lineTo(c.x() - 0.12 * r, c.y() + 0.35 * r);
        check.lineTo(c.x() + 0.48 * r, c.y() - 0.32 * r);
        p.drawPath(check);
        break;
    }
    case HoldStrip::Icon::Canceled:
        p.drawLine(QPointF(c.x() - 0.36 * r, c.y() - 0.36 * r), QPointF(c.x() + 0.36 * r, c.y() + 0.36 * r));
        p.drawLine(QPointF(c.x() + 0.36 * r, c.y() - 0.36 * r), QPointF(c.x() - 0.36 * r, c.y() + 0.36 * r));
        break;
    }
}

void drawWave(QPainter& p, const QRectF& area, const QVector<float>& bins, const QColor& on, const QColor& off)
{
    const int   count  = qMax(0, static_cast<int>((area.width() + kBarGap) / (kBar + kBarGap)));
    const qreal center = area.center().y();
    const qreal range  = area.height() - 2.0;
    const int   have   = bins.size();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) { // newest at the right; places not recorded yet are small dots
        const int   bin = have - count + i;
        const bool  set = bin >= 0;
        const qreal h   = set ? 2.0 + range * waveform::liveHeight(bins.at(bin)) : 2.0;
        p.setBrush(set ? on : off);
        p.drawRoundedRect(QRectF(area.left() + i * (kBar + kBarGap), center - h / 2.0, kBar, h), kBar / 2.0, kBar / 2.0);
    }
}

} // namespace

HoldStrip::~HoldStrip()
{
    if (m_input)
        m_input->removeEventFilter(this);
}

bool HoldStrip::eventFilter(QObject* watched, QEvent* event)
{
    // Its input went out of sight (another server tab, TeamSpeak minimized or closing): never over another
    // chat, and never left behind on its own.
    if (watched == m_input && event->type() == QEvent::Hide && isVisible())
        hide();
    return QWidget::eventFilter(watched, event);
}

HoldStrip::HoldStrip(QWidget* parent)
    : QWidget(parent)
{
    // fromLatin1: it lives among TeamSpeak's widgets; plugin shutdown finds it by this name.
    setObjectName(QString::fromLatin1("tsmediaHoldStrip"));
    setAttribute(Qt::WA_TransparentForMouseEvents); // the button keeps the press, the chat its clicks
    setAttribute(Qt::WA_QuitOnClose, false);        // never one of the windows that keep TeamSpeak running
    setFocusPolicy(Qt::NoFocus);
    setLayoutDirection(Qt::LeftToRight);
    setAccessibleName(i18n::t("Voice message"));
    m_frame = new QTimer(this);
    m_frame->setInterval(kFrameMs);
    connect(m_frame, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    m_clock.start();
}

QString HoldStrip::line(const View& view)
{
    switch (view.mode) {
    case Mode::Recording:
        return view.problem.isEmpty() ? hintText(false) : view.problem;
    case Mode::Cancel:
        return i18n::t("Release to cancel");
    case Mode::Saving:
    case Mode::Notice:
        break;
    }
    return view.text;
}

void HoldStrip::setView(const View& view)
{
    const bool modeChanged = view.mode != m_view.mode;
    m_view                 = view;
    updateClock();
    // Screen readers hear what to do (politely: the focus stays in the input), not the time ticking.
    const QString said = line(view);
    if (said != m_announced) {
        m_announced = said;
        setAccessibleDescription(said);
        if (QAccessible::isActive() && (modeChanged || view.mode == Mode::Notice)) {
            QAccessibleEvent event(this, QAccessible::DescriptionChanged);
            QAccessible::updateAccessibility(&event);
        }
    }
    update();
}

int HoldStrip::heightFor(const QFont& inputFont)
{
    return qMax(32, static_cast<int>(std::ceil(QFontMetricsF(timeFont(inputFont)).height())) + 14);
}

void HoldStrip::placeAbove(QWidget* input)
{
    if (!input)
        return;
    if (m_input != input) {
        if (m_input)
            m_input->removeEventFilter(this);
        m_input = input;
        input->installEventFilter(this); // it hides with the input
    }
    QWidget* owner = input->window();
    if (parentWidget() != owner) {
        const bool shown = isVisible();
        setParent(owner); // hidden by setParent
        if (shown)
            show();
    }
    const QFont font = input->font();
    if (this->font() != font)
        setFont(font);
    const int    height = heightFor(font);
    const QPoint at     = input->mapTo(owner, QPoint(0, 0));
    const QRect  place(at.x(), qMax(0, at.y() - height), input->width(), height);
    if (geometry() != place)
        setGeometry(place);
    const QObjectList& siblings = owner->children();
    if (siblings.isEmpty() || siblings.last() != this) // over the chat (raise() repaints, so only when needed)
        raise();
}

HoldStrip::Look HoldStrip::look() const
{
    Look l;
    const QWidget* source = m_input ? m_input.data() : this;
    const QPalette pal    = source->palette();
    l.base                = pal.color(QPalette::Base);
    l.dark                = l.base.lightness() < 128 || pal.color(QPalette::Text).lightness() > 170; // as the mic button
    l.font                = font();
    if (m_frame->isActive())
        l.phase = static_cast<qreal>(m_clock.elapsed() % kPeriodMs) / static_cast<qreal>(kPeriodMs);
    return l;
}

void HoldStrip::updateClock()
{
    const bool moving = isVisible() && m_view.animate && (m_view.mode == Mode::Recording || m_view.mode == Mode::Saving) && ui::animationsEnabled();
    if (moving == m_frame->isActive())
        return;
    if (moving)
        m_frame->start();
    else
        m_frame->stop();
}

void HoldStrip::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateClock();
}

void HoldStrip::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateClock();
}

void HoldStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    render(p, QRectF(rect()), m_view, look());
}

void HoldStrip::render(QPainter& p, const QRectF& r, const View& view, const Look& look)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const Colors c = colorsFor(look, view.mode);

    // Attached to the input: rounded top corners, square at the bottom (the reply bar's shape), and a
    // hairline along the top that sets it off the chat.
    QPainterPath bg;
    bg.moveTo(r.left(), r.bottom());
    bg.lineTo(r.left(), r.top() + kRadius);
    bg.quadTo(r.left(), r.top(), r.left() + kRadius, r.top());
    bg.lineTo(r.right() - kRadius, r.top());
    bg.quadTo(r.right(), r.top(), r.right(), r.top() + kRadius);
    bg.lineTo(r.right(), r.bottom());
    bg.closeSubpath();
    p.fillPath(bg, c.surface);
    QPainterPath edge;
    edge.moveTo(r.left() + 0.5, r.bottom());
    edge.lineTo(r.left() + 0.5, r.top() + kRadius);
    edge.quadTo(r.left() + 0.5, r.top() + 0.5, r.left() + kRadius, r.top() + 0.5);
    edge.lineTo(r.right() - kRadius, r.top() + 0.5);
    edge.quadTo(r.right() - 0.5, r.top() + 0.5, r.right() - 0.5, r.top() + kRadius);
    edge.lineTo(r.right() - 0.5, r.bottom());
    p.setPen(QPen(c.line, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(edge);

    const QFont         small = hintFont(look.font);
    const QFont         bold  = timeFont(look.font);
    const QFontMetricsF sm(small), bm(bold);
    const qreal         cy    = r.center().y();
    const auto baseline = [cy](const QFontMetricsF& fm) { return std::round(cy + (fm.ascent() - fm.descent()) / 2.0); };
    qreal               x     = r.left() + kPad;
    const qreal         right = r.right() - kPad;

    if (view.mode == Mode::Notice) {
        const QRectF icon(x, cy - 8.0, 16.0, 16.0);
        drawNoticeIcon(p, icon, view.icon, view.icon == Icon::Canceled ? c.muted : c.accent);
        x += 16.0 + 8.0;
        p.setFont(small);
        p.setPen(c.text);
        p.drawText(QPointF(x, baseline(sm)), sm.elidedText(view.text, Qt::ElideRight, qMax(0.0, right - x)));
        p.restore();
        return;
    }

    // The mark: the dot (breathing while it records), the bin in the cancel zone, the spinner when sent.
    const QRectF mark(x, cy - kMark / 2.0, kMark, kMark);
    switch (view.mode) {
    case Mode::Recording: {
        qreal opacity = 1.0;
        if (look.phase >= 0.0)
            opacity = 0.45 + 0.55 * (0.5 + 0.5 * std::cos(2.0 * M_PI * look.phase));
        p.setPen(Qt::NoPen);
        QColor dot = c.record;
        dot.setAlphaF(opacity);
        p.setBrush(dot);
        p.drawEllipse(mark.center(), 5.0, 5.0);
        break;
    }
    case Mode::Cancel:
        drawBin(p, mark.adjusted(0.5, -0.5, -0.5, 0.5), c.error);
        break;
    case Mode::Saving: {
        p.setPen(QPen(c.quiet, 1.8));
        p.setBrush(Qt::NoBrush);
        const QRectF ring = mark.adjusted(1.5, 1.5, -1.5, -1.5);
        p.drawEllipse(ring);
        p.setPen(QPen(c.accent, 1.8, Qt::SolidLine, Qt::RoundCap));
        const int start = look.phase >= 0.0 ? static_cast<int>(-look.phase * 360.0 * 16.0) : 90 * 16;
        p.drawArc(ring, start, -100 * 16);
        break;
    }
    case Mode::Notice:
        break;
    }
    x += kMark + 8.0;

    // The time and the limit.
    const QString time  = formatDuration(view.timeMs);
    const QString limit = i18n::t(" / %1").arg(formatDuration(view.limitMs));
    p.setFont(bold);
    p.setPen(view.timeWarning && view.mode != Mode::Cancel ? c.error : c.text);
    p.drawText(QPointF(x, baseline(bm)), time);
    x += bm.horizontalAdvance(time);
    p.setFont(small);
    p.setPen(c.muted);
    p.drawText(QPointF(x, baseline(bm)), limit);
    x += sm.horizontalAdvance(limit) + kGap;

    // The line at the right, then the waveform in what is left.
    QString     text   = line(view);
    const bool  strong = view.mode == Mode::Cancel;
    const bool  alert  = strong || (view.mode == Mode::Recording && !view.problem.isEmpty());
    QFont       font   = small;
    if (strong)
        font.setWeight(QFont::DemiBold);
    QFontMetricsF fm(font);
    qreal         width = fm.horizontalAdvance(text);
    if (view.mode == Mode::Recording && view.problem.isEmpty() && right - x - width - kGap < kMinWave) {
        text  = hintText(true);
        width = fm.horizontalAdvance(text);
    }
    qreal waveRight = right - width - kGap;
    if (waveRight - x < kMinWave / 2.0) { // no room for a waveform: the line gets it all
        waveRight = x - kGap;
        text      = fm.elidedText(text, Qt::ElideRight, qMax(0.0, right - x));
        width     = fm.horizontalAdvance(text);
    }
    if (waveRight > x) {
        const QRectF area(x, r.top() + 8.0, waveRight - x, r.height() - 16.0);
        const bool   live = view.mode == Mode::Recording;
        drawWave(p, area, view.liveBins, live ? c.live : c.quiet, live ? ui::flatten(QColor(c.quiet.red(), c.quiet.green(), c.quiet.blue(), 130), c.surface) : c.quiet);
    }
    p.setFont(font);
    p.setPen(alert ? c.error : c.muted);
    p.drawText(QPointF(right - width, baseline(fm)), text);
    p.restore();
}
