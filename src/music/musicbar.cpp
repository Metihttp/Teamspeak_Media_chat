#include "music/musicbar.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAccessible>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QStringList>
#include <QTextLayout>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>
#include <functional>
#include <limits>

#include "i18n.h"
#include "medialink.h" // formatDuration
#include "replyart.h"
#include "uiutil.h"

namespace music {

namespace {

constexpr int    kPadLeft  = 10;
constexpr int    kPadRight = 6;
constexpr int    kBadge    = 24;
constexpr int    kButton   = 28; // hit targets
constexpr int    kGap      = 8;
constexpr int    kTight    = 4;  // between icon buttons
constexpr int    kTitleMin  = 64;
constexpr int    kTitleWant = 120; // what the title keeps before the optional parts go
constexpr int    kNoticeLines = 3;
constexpr int    kSeekMin  = 56;
constexpr int    kVolumeW  = 64;
constexpr qreal  kRadius   = 6.0;

QFont titleFont(const QFont& input)
{
    QFont font = replyart::scaledFont(input, 1.0);
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont smallFont(const QFont& input)
{
    return replyart::scaledFont(input, 0.92);
}

int rowHeight(const QFont& font)
{
    return qMax(38, static_cast<int>(std::ceil(QFontMetricsF(titleFont(font)).height())) + 20);
}

int problemHeight(const QFont& font)
{
    return qMax(20, static_cast<int>(std::ceil(QFontMetricsF(smallFont(font)).height())) + 6);
}

// A notice's text in at most kNoticeLines lines (the last one elided), as wide as width.
QStringList noticeLines(const QFont& font, const QString& text, qreal width)
{
    QStringList         lines;
    const QFontMetricsF fm(smallFont(font));
    if (width <= 0 || text.isEmpty())
        return {text};
    QTextLayout layout(text, smallFont(font));
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (lines.size() == kNoticeLines - 1) { // the rest on the last line, elided
            lines << fm.elidedText(text.mid(line.textStart()).simplified(), Qt::ElideRight, width);
            break;
        }
        lines << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines.isEmpty() ? QStringList{text} : lines;
}

int noticeTextWidth(int barWidth)
{
    return qMax(40, barWidth - kPadLeft - 16 - kGap - kGap - kButton - kPadRight);
}

int timeWidth(const QFont& font, qint64 durationMs)
{
    const QFontMetricsF fm(smallFont(font));
    const QString       sample = durationMs >= 3600000 ? QStringLiteral("8:88:88") : durationMs >= 600000 ? QStringLiteral("88:88") : QStringLiteral("8:88");
    return static_cast<int>(std::ceil(fm.horizontalAdvance(sample))) + 2;
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

// ---- glyphs (the stroke style of the plugin's other icons) -----------------------------------------------

enum class Glyph { Play, Pause, Stop, Volume, VolumeLow, Muted, Close };

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    p.save();
    const QPointF c = box.center();
    const qreal   s = qMin(box.width(), box.height());
    switch (glyph) {
    case Glyph::Play: {
        QPainterPath tri;
        const qreal h = s * 0.62, w = s * 0.56;
        const qreal x = c.x() - w * 0.38; // optically centred
        tri.moveTo(x, c.y() - h / 2.0);
        tri.lineTo(x + w, c.y());
        tri.lineTo(x, c.y() + h / 2.0);
        tri.closeSubpath();
        p.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(tri);
        break;
    }
    case Glyph::Pause: {
        const qreal h = s * 0.6, w = s * 0.18, gap = s * 0.14;
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(c.x() - gap / 2.0 - w, c.y() - h / 2.0, w, h), w / 3.0, w / 3.0);
        p.drawRoundedRect(QRectF(c.x() + gap / 2.0, c.y() - h / 2.0, w, h), w / 3.0, w / 3.0);
        break;
    }
    case Glyph::Stop: {
        const qreal side = s * 0.56; // (0.46 read as a dot at 1x next to the other icons)
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(c.x() - side / 2.0, c.y() - side / 2.0, side, side), side * 0.2, side * 0.2);
        break;
    }
    case Glyph::Volume:
    case Glyph::VolumeLow:
    case Glyph::Muted: {
        const qreal  u = s / 16.0;
        const qreal  x = c.x() - 6.5 * u, y = c.y();
        QPainterPath speaker;
        speaker.moveTo(x, y - 2.2 * u);
        speaker.lineTo(x + 2.6 * u, y - 2.2 * u);
        speaker.lineTo(x + 6.0 * u, y - 5.2 * u);
        speaker.lineTo(x + 6.0 * u, y + 5.2 * u);
        speaker.lineTo(x + 2.6 * u, y + 2.2 * u);
        speaker.lineTo(x, y + 2.2 * u);
        speaker.closeSubpath();
        p.setPen(QPen(color, 1.1 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(speaker);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 1.5 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (glyph == Glyph::Muted) {
            const qreal mx = x + 10.0 * u;
            p.drawLine(QPointF(mx, y - 2.6 * u), QPointF(mx + 5.0 * u, y + 2.4 * u));
            p.drawLine(QPointF(mx + 5.0 * u, y - 2.6 * u), QPointF(mx, y + 2.4 * u));
        } else {
            const QRectF near(x + 4.5 * u, y - 3.5 * u, 7.0 * u, 7.0 * u);
            p.drawArc(near, -50 * 16, 100 * 16);
            if (glyph == Glyph::Volume) {
                const QRectF far(x + 3.5 * u, y - 6.5 * u, 13.0 * u, 13.0 * u);
                p.drawArc(far, -50 * 16, 100 * 16);
            }
        }
        break;
    }
    case Glyph::Close: {
        const qreal d = s * 0.2;
        p.setPen(QPen(color, qMax(1.5, s / 11.0), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
        p.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
        break;
    }
    }
    p.restore();
}

// Two beamed eighth notes in a 24-unit box: the attach menu's entry, in the stroke weight of that menu's
// other glyphs (attach-poll's attachart: 1.8 units), centred like them.
void drawBeamedNotes(QPainter& p, const QRectF& box, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    const qreal scale = qMin(box.width(), box.height()) / 24.0;
    p.scale(scale, scale);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (const QPointF& head : {QPointF(7.3, 18.2), QPointF(16.3, 16.2)}) {
        p.save();
        p.translate(head);
        p.rotate(-20);
        p.drawEllipse(QPointF(0, 0), 3.0, 2.3);
        p.restore();
    }
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(9.9, 17.6), QPointF(9.9, 6.6));
    p.drawLine(QPointF(18.9, 15.6), QPointF(18.9, 4.6));
    QPainterPath beam;
    beam.moveTo(9.0, 5.6);
    beam.lineTo(19.8, 3.4);
    beam.lineTo(19.8, 6.0);
    beam.lineTo(9.0, 8.2);
    beam.closeSubpath();
    p.setPen(Qt::NoPen);
    p.fillPath(beam, color);
    p.restore();
}

// The warning triangle and the info circle of the problem line and the notice.
void drawMark(QPainter& p, const QRectF& box, bool warning, const QColor& color)
{
    p.save();
    const QPointF c = box.center();
    const qreal   r = box.width() / 2.0;
    p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (warning) {
        QPainterPath tri;
        tri.moveTo(c.x(), c.y() - r * 0.9);
        tri.lineTo(c.x() + r * 0.95, c.y() + r * 0.8);
        tri.lineTo(c.x() - r * 0.95, c.y() + r * 0.8);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawLine(QPointF(c.x(), c.y() - r * 0.3), QPointF(c.x(), c.y() + r * 0.2));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(c.x(), c.y() + r * 0.5), 0.95, 0.95);
    } else {
        p.drawEllipse(c, r - 0.7, r - 0.7);
        p.drawLine(QPointF(c.x(), c.y() - r * 0.05), QPointF(c.x(), c.y() + r * 0.45));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(c.x(), c.y() - r * 0.42), 0.95, 0.95);
    }
    p.restore();
}

void drawFocusRing(QPainter& p, const QRectF& r, qreal radius, const QColor& color)
{
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, 2.0));
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius, radius);
    p.restore();
}

} // namespace

// ---- the buttons ------------------------------------------------------------------------------------------

class BarButton : public QAbstractButton
{
  public:
    BarButton(MusicBar* bar, bool primary)
        : QAbstractButton(bar)
        , m_bar(bar)
        , m_primary(primary)
    {
        setFocusPolicy(Qt::TabFocus); // a click leaves the focus in the chat input
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    void setGlyph(Glyph glyph)
    {
        if (m_glyph != glyph) {
            m_glyph = glyph;
            update();
        }
    }
    std::function<void(int)> step; // the volume button: wheel and arrow keys change the volume

    QSize sizeHint() const override { return QSize(kButton, kButton); }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const MusicBar::Colors& c    = m_bar->colors();
        const QRectF            r    = QRectF(rect());
        const bool              hot  = underMouse() && isEnabled();
        const bool              down = isDown();
        if (!isEnabled())
            p.setOpacity(0.4);
        QColor ink = c.text;
        if (m_primary) {
            const qreal  d    = qMin(r.width(), r.height()) - 4.0;
            const QRectF disc(r.center().x() - d / 2.0, r.center().y() - d / 2.0, d, d);
            QColor       fill = c.accent;
            if (down)
                fill = fill.darker(115);
            else if (hot)
                fill = m_bar->dark() ? fill.lighter(112) : fill.darker(108);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(disc);
            ink = c.onAccent;
            drawGlyph(p, m_glyph, disc.adjusted(d * 0.2, d * 0.2, -d * 0.2, -d * 0.2), ink);
            if (hasFocus())
                drawFocusRing(p, r.adjusted(-0.5, -0.5, 0.5, 0.5), r.height() / 2.0, c.focus);
            return;
        }
        if (hot || down) {
            p.setPen(Qt::NoPen);
            p.setBrush(down ? c.pressed : c.hover);
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
        }
        ink = hot || down ? c.text : c.muted;
        const qreal g = m_glyph == Glyph::Close ? 14.0 : 16.0;
        drawGlyph(p, m_glyph, QRectF(r.center().x() - g / 2.0, r.center().y() - g / 2.0, g, g), ink);
        if (hasFocus())
            drawFocusRing(p, r.adjusted(1, 1, -1, -1), 6, c.focus);
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            animateClick(0);
            return;
        }
        if (step && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Right)) {
            step(5);
            return;
        }
        if (step && (event->key() == Qt::Key_Down || event->key() == Qt::Key_Left)) {
            step(-5);
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override
    {
        if (!step) {
            QAbstractButton::wheelEvent(event);
            return;
        }
        const int delta = event->angleDelta().y();
        if (delta != 0)
            step(delta > 0 ? 5 : -5);
        event->accept();
    }

  private:
    MusicBar* m_bar;
    bool      m_primary;
    Glyph     m_glyph = Glyph::Play;
};

// ---- the sliders -----------------------------------------------------------------------------------------

class BarSlider : public QAbstractSlider
{
  public:
    explicit BarSlider(MusicBar* bar)
        : QAbstractSlider(bar)
        , m_bar(bar)
    {
        setOrientation(Qt::Horizontal);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setTracking(true);
    }
    QSize sizeHint() const override { return QSize(120, kButton); }
    bool  wheelMoves = true; // false (the position): the wheel isn't a seek, the thumb doesn't move for it
    void  setDimmed(bool dimmed) // the music is muted: the level stays, drawn quiet
    {
        if (m_dimmed != dimmed) {
            m_dimmed = dimmed;
            update();
        }
    }

  protected:
    QRectF track() const
    {
        const qreal inset = 7.0; // the thumb never leaves the widget
        return QRectF(inset, height() / 2.0 - 2.0, qMax(1.0, width() - 2.0 * inset), 4.0);
    }
    int valueAt(qreal x) const
    {
        const QRectF t = track();
        if (maximum() <= minimum())
            return minimum();
        const qreal f = qBound(0.0, (x - t.left()) / t.width(), 1.0);
        return minimum() + static_cast<int>(std::lround(f * (maximum() - minimum())));
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const MusicBar::Colors& c = m_bar->colors();
        if (!isEnabled())
            p.setOpacity(0.45);
        const QRectF t    = track();
        const qreal  span = maximum() > minimum() ? static_cast<qreal>(sliderPosition() - minimum()) / (maximum() - minimum()) : 0.0;
        const qreal  x    = t.left() + t.width() * qBound(0.0, span, 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(c.quiet);
        p.drawRoundedRect(t, 2, 2);
        const QColor fill = m_dimmed ? c.muted : c.accent;
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(t.left(), t.top(), x - t.left(), t.height()), 2, 2);
        const bool  hot    = (underMouse() || isSliderDown()) && isEnabled();
        const qreal radius = isSliderDown() ? 7.0 : hot || hasFocus() ? 6.0 : 4.5;
        if (isEnabled()) {
            p.setBrush(m_bar->dark() ? c.text : QColor(Qt::white));
            p.setPen(QPen(fill, isSliderDown() || hot ? 2.0 : 1.6));
            p.drawEllipse(QPointF(x, t.center().y()), radius, radius);
        }
        if (hasFocus()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(c.focus, 2.0));
            p.drawEllipse(QPointF(x, t.center().y()), radius + 3.0, radius + 3.0);
        }
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || maximum() <= minimum()) {
            event->ignore();
            return;
        }
        setSliderDown(true);
        setSliderPosition(valueAt(event->localPos().x())); // a click jumps there, a drag goes on from it
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!isSliderDown()) {
            event->ignore();
            return;
        }
        setSliderPosition(valueAt(event->localPos().x()));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !isSliderDown()) {
            event->ignore();
            return;
        }
        setSliderPosition(valueAt(event->localPos().x()));
        setSliderDown(false); // sliderReleased: the seek
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override
    {
        if (!wheelMoves) { // (QAbstractSlider would move the thumb as a SliderMove, never asking for a seek)
            event->ignore();
            return;
        }
        QAbstractSlider::wheelEvent(event);
    }
    void enterEvent(QEvent* event) override
    {
        QAbstractSlider::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override
    {
        QAbstractSlider::leaveEvent(event);
        update();
    }

  private:
    MusicBar* m_bar;
    bool      m_dimmed = false;
};

} // namespace music

using namespace music; // the sizes, fonts and drawing helpers above, the controls

// ---- MusicBar ---------------------------------------------------------------------------------------------

MusicBar::Colors MusicBar::colorsFor(bool dark, const QColor& baseIn)
{
    const QColor            base = baseIn.isValid() && baseIn.alpha() == 255 ? baseIn : (dark ? QColor(0x31, 0x33, 0x38) : QColor(Qt::white));
    const replyart::Palette pal  = replyart::paletteFor(dark, base);
    Colors                  c;
    c.surface  = pal.surface;
    c.line     = ui::flatten(dark ? QColor(255, 255, 255, 24) : QColor(0, 0, 0, 22), base);
    c.text     = replyart::readable(pal.text, c.surface, pal.text, 7.0);
    c.muted    = replyart::readable(pal.muted, c.surface, pal.muted, 4.5);
    c.accent   = replyart::readable(dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x58, 0x65, 0xf2), c.surface, c.text, 3.0);
    c.onAccent = ui::contrastRatio(Qt::white, c.accent) >= 3.0 ? QColor(Qt::white) : QColor(0x1e, 0x1f, 0x22);
    c.hover    = ui::flatten(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 14), c.surface);
    c.pressed  = ui::flatten(dark ? QColor(255, 255, 255, 34) : QColor(0, 0, 0, 24), c.surface);
    c.quiet    = ui::flatten(dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 34), c.surface);
    c.error    = replyart::readable(dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d), c.surface, c.text, 4.5);
    c.focus    = c.accent;
    c.note     = replyart::readable(menuHue(), c.surface, c.accent, 3.0); // the paperclip menu's music note
    // 2.3 ux review: the note sits on its tile, not on the bar: 3:1 there (it was 2.9:1).
    c.noteTile = ui::flatten(withAlpha(c.note, dark ? 46 : 34), c.surface);
    c.note     = ui::readableOn(c.note, {c.noteTile}, 3.0);
    return c;
}

MusicBar::MusicBar(QWidget* parent)
    : QWidget(parent)
{
    // fromLatin1: it lives among TeamSpeak's widgets; plugin shutdown and the stack find it by this name.
    setObjectName(QString::fromLatin1("tsmediaMusicBar"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::NoFocus);
    setLayoutDirection(Qt::LeftToRight);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAccessibleName(i18n::t("Live music"));
    setMouseTracking(true);
    m_colors = colorsFor(false, QColor());

    m_play = new BarButton(this, true);
    connect(m_play, &QAbstractButton::clicked, this, &MusicBar::playPauseClicked);
    m_stop = new BarButton(this, false);
    m_stop->setGlyph(Glyph::Stop);
    m_stop->setAccessibleName(i18n::t("Stop"));
    m_stop->setToolTip(i18n::t("Stop the music (the bar goes away)"));
    connect(m_stop, &QAbstractButton::clicked, this, &MusicBar::stopClicked);
    m_close = new BarButton(this, false);
    m_close->setGlyph(Glyph::Close);
    m_close->setAccessibleName(i18n::t("Close"));
    m_close->setToolTip(i18n::t("Close"));
    connect(m_close, &QAbstractButton::clicked, this, &MusicBar::noticeClosed);
    m_volumeButton = new BarButton(this, false);
    m_volumeButton->step = [this](int delta) {
        if (m_view.muted && delta > 0)
            emit muteToggled();
        emit volumeChanged(qBound(0, m_view.volume + delta, 100));
    };
    connect(m_volumeButton, &QAbstractButton::clicked, this, &MusicBar::muteToggled);

    m_seek = new BarSlider(this);
    m_seek->wheelMoves = false;
    m_seek->setAccessibleName(i18n::t("Position"));
    m_seek->setSingleStep(5000);
    m_seek->setPageStep(30000);
    m_seek->setToolTip(i18n::t("Drag to move forward or back (arrow keys: 5 seconds)"));
    connect(m_seek, &QAbstractSlider::sliderMoved, this, [this] { update(); }); // the time follows the drag
    connect(m_seek, &QAbstractSlider::sliderReleased, this, [this] { emit seekRequested(m_seek->sliderPosition()); });
    connect(m_seek, &QAbstractSlider::actionTriggered, this, [this](int action) {
        if (action != QAbstractSlider::SliderMove && action != QAbstractSlider::SliderNoAction && !m_seek->isSliderDown())
            emit seekRequested(m_seek->sliderPosition()); // keys (the wheel doesn't seek)
    });

    m_volume = new BarSlider(this);
    m_volume->setRange(0, 100);
    m_volume->setSingleStep(5);
    m_volume->setPageStep(20);
    m_volume->setAccessibleName(i18n::t("Volume"));
    connect(m_volume, &QAbstractSlider::valueChanged, this, [this](int value) {
        if (value != m_view.volume)
            emit volumeChanged(value);
    });

    applyTexts();
}

MusicBar::~MusicBar() = default;

QWidget* MusicBar::playButton() const { return m_play; }
QWidget* MusicBar::stopButton() const { return m_stop; }
QWidget* MusicBar::closeButton() const { return m_close; }
QWidget* MusicBar::seekSlider() const { return m_seek; }
QWidget* MusicBar::volumeSlider() const { return m_volume; }
QWidget* MusicBar::volumeButton() const { return m_volumeButton; }

int MusicBar::heightFor(const QFont& font, bool withProblem)
{
    return rowHeight(font) + (withProblem ? problemHeight(font) : 0);
}

QSize MusicBar::sizeHint() const
{
    if (m_view.mode == Mode::Notice)
        return QSize(480, heightForWidth(width() > 0 ? width() : 480));
    return QSize(480, heightFor(font(), !m_view.problem.isEmpty()));
}

bool MusicBar::hasHeightForWidth() const
{
    return m_view.mode == Mode::Notice; // a long notice wraps
}

int MusicBar::heightForWidth(int width) const
{
    if (m_view.mode != Mode::Notice)
        return sizeHint().height();
    const int lines = static_cast<int>(noticeLines(font(), m_view.notice, noticeTextWidth(width)).size());
    if (lines <= 1)
        return rowHeight(font()); // one line: as high as the playing bar
    const int line = static_cast<int>(std::ceil(QFontMetricsF(smallFont(font())).lineSpacing()));
    return qMax(rowHeight(font()), lines * line + 20);
}

QSize MusicBar::minimumSizeHint() const
{
    return QSize(160, sizeHint().height());
}

bool MusicBar::seeking() const
{
    return m_seek->isSliderDown();
}

qint64 MusicBar::shownPositionMs() const
{
    return m_seek->isSliderDown() ? m_seek->sliderPosition() : m_view.positionMs;
}

MusicBar::Geometry MusicBar::geometryFor(const QSize& size, const QFont& font, const View& view)
{
    Geometry   g;
    const int  row   = rowHeight(font);
    const int  cy    = row / 2;
    const int  width = size.width();
    const auto box   = [cy](int x, int w, int h) { return QRect(x, cy - h / 2, w, h); };
    if (!view.problem.isEmpty() && view.mode != Mode::Notice)
        g.problem = QRect(kPadLeft, row - 4, qMax(0, width - kPadLeft - kPadRight), problemHeight(font));

    if (view.mode == Mode::Notice) { // the icon and the close button by the first line, the text as high as it needs
        const qreal spacing = QFontMetricsF(smallFont(font)).lineSpacing();
        const int   center  = size.height() > row ? static_cast<int>(std::round(10.0 + spacing / 2.0)) : cy;
        g.badge             = QRect(kPadLeft, center - 8, 16, 16);
        g.close             = QRect(width - kPadRight - kButton, center - kButton / 2, kButton, kButton);
        g.title = QRect(g.badge.right() + 1 + kGap, 0, noticeTextWidth(width), qMax(row, size.height()));
        g.showElapsed = g.showTotal = g.showSeek = g.showVolumeSlider = g.showVolumeButton = false;
        return g;
    }

    if (!view.away.isEmpty()) {
        // 2.3: on another server tab: the note, the title and where it plays, play / pause and stop (and the
        // line below when it waits for its tab: "Paused while you're on another server tab…").
        g.showElapsed = g.showTotal = g.showSeek = g.showVolumeSlider = g.showVolumeButton = false;
        int x         = kPadLeft;
        g.badge       = box(x, kBadge, kBadge);
        x += kBadge + kGap;
        g.stop  = box(width - kPadRight - kButton, kButton, kButton);
        g.play  = box(g.stop.left() - kTight - kButton, kButton, kButton);
        g.title = QRect(x, 0, qMax(0, g.play.left() - kGap - x), row);
        return g;
    }

    const bool          seekable = view.durationMs > 0 && view.mode != Mode::Loading;
    const int           tw       = timeWidth(font, qMax(view.durationMs, view.positionMs));
    const QFontMetricsF tm(titleFont(font)), sm(smallFont(font));
    const qreal         titleOnly = tm.horizontalAdvance(view.title);
    qreal               natural   = titleOnly;
    const QString       subtitle  = view.mode == Mode::Loading && view.artist.isEmpty() ? i18n::t("Loading…") : view.artist;
    if (!subtitle.isEmpty())
        natural += sm.horizontalAdvance(QStringLiteral("  ·  ") + subtitle);
    // The title (not the artist) keeps up to kTitleWant of what it needs before anything optional goes;
    // then the parts go in this order: the volume slider, the total, the volume button, the seek slider,
    // the time.
    const int titleWant = qBound(kTitleMin, static_cast<int>(std::ceil(titleOnly)), kTitleWant);
    const int fixedBase = kPadLeft + kBadge + kGap + kGap + kButton + kGap + kTight + kButton + kPadRight;
    struct Part {
        bool* shown;
        int   width;
    };
    g.showTotal  = seekable;
    g.showSeek   = seekable;
    Part parts[] = {{&g.showVolumeSlider, kVolumeW}, {&g.showTotal, tw + kGap}, {&g.showVolumeButton, kButton},
                    {&g.showSeek, kSeekMin + kGap}, {&g.showElapsed, tw + kGap}};
    int  optional = 0;
    for (const Part& part : parts)
        optional += *part.shown ? part.width : 0;
    for (const Part& part : parts) {
        if (fixedBase + titleWant + optional <= width)
            break;
        if (*part.shown) {
            *part.shown = false;
            optional -= part.width;
        }
    }
    // The room left: the title gets what it still needs (up to 45% of it), the seek slider the rest.
    const int spare     = width - fixedBase - optional;
    const int titleW    = qMax(kTitleMin, qMin(titleWant, spare));
    const int rest      = qMax(0, spare - titleW);
    const int titleNeed = qMax(0, static_cast<int>(std::ceil(natural)) - titleW);
    // (Without a seek slider the title takes only what it needs: play stays next to it.)
    const int titleMore = g.showSeek ? qMin(titleNeed, qMax(rest * 45 / 100, rest - 360)) : qMin(titleNeed, rest);
    const int seekMore  = g.showSeek ? rest - titleMore : 0;

    int x   = kPadLeft;
    g.badge = box(x, kBadge, kBadge);
    x += kBadge + kGap;
    g.title = QRect(x, 0, titleW + titleMore, row);
    x += g.title.width() + kGap;
    g.play = box(x, kButton, kButton);
    x += kButton + kGap;
    if (g.showElapsed) {
        g.elapsed = QRect(x, 0, tw, row);
        x += tw + kGap;
    }
    if (g.showSeek) { // the thumb's room reaches half a gap into each side: the track sits a gap from the times
        g.seek = box(x - kGap / 2, kSeekMin + seekMore + kGap, kButton);
        x += kSeekMin + seekMore + kGap;
    }
    if (g.showTotal) {
        g.total = QRect(x, 0, tw, row);
        x += tw + kGap;
    }
    // The volume and stop at the right edge.
    int right = width - kPadRight;
    g.stop    = box(right - kButton, kButton, kButton);
    right -= kButton + kTight;
    if (g.showVolumeSlider) {
        g.volumeSlider = box(right - kVolumeW, kVolumeW, kButton);
        right -= kVolumeW;
    }
    if (g.showVolumeButton)
        g.volumeButton = box(right - kButton, kButton, kButton);
    return g;
}

void MusicBar::setTheme(bool dark, const QColor& base, const QFont& font)
{
    if (dark == m_dark && base == m_base && font == this->font())
        return;
    m_dark   = dark;
    m_base   = base;
    m_colors = colorsFor(dark, base);
    setFont(font);
    updateGeometry(); // the layout takes sizeHint()'s height (a fixed vertical policy, no fixed size: no
                      // size constraints on a bar that isn't in TeamSpeak's window yet)
    relayout();
    update();
}

void MusicBar::setView(const View& view)
{
    const bool modeChanged = view.mode != m_view.mode;
    const int  oldHeight   = sizeHint().height();
    const bool oldWraps    = hasHeightForWidth();
    m_view                 = view;
    m_view.volume          = qBound(0, m_view.volume, 100);
    if (sizeHint().height() != oldHeight || hasHeightForWidth() != oldWraps)
        updateGeometry(); // a line more or less (only then: TeamSpeak's window lays out again)
    // The sliders follow the music unless they are being dragged.
    const int duration = static_cast<int>(qBound<qint64>(0, m_view.durationMs, std::numeric_limits<int>::max()));
    if (!m_seek->isSliderDown()) {
        const QSignalBlocker block(m_seek);
        m_seek->setRange(0, duration);
        m_seek->setValue(static_cast<int>(qBound<qint64>(0, m_view.positionMs, duration)));
    }
    if (!m_volume->isSliderDown() && m_volume->value() != m_view.volume) {
        const QSignalBlocker block(m_volume);
        m_volume->setValue(m_view.volume);
    }
    m_seek->setEnabled(duration > 0 && m_view.mode != Mode::Loading);
    m_play->setEnabled(m_view.mode != Mode::Loading);
    m_play->setGlyph(m_view.mode == Mode::Playing ? Glyph::Pause : Glyph::Play);
    m_volumeButton->setGlyph(m_view.muted || m_view.volume == 0 ? Glyph::Muted : m_view.volume < 40 ? Glyph::VolumeLow : Glyph::Volume);
    m_volume->setDimmed(m_view.muted);
    applyTexts();
    relayout();
    if (modeChanged && QAccessible::isActive()) {
        QAccessibleEvent event(this, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
    update();
}

void MusicBar::applyTexts()
{
    const bool playing = m_view.mode == Mode::Playing;
    const bool away    = !m_view.away.isEmpty();
    m_play->setAccessibleName(playing ? i18n::t("Pause") : i18n::t("Play"));
    // 2.3 live test: on another server tab, Play brings the music's tab to the front (it plays on there).
    const QString goBack = away && !playing ? i18n::t("Back to %1 and play").arg(m_view.away) : QString();
    m_play->setToolTip(playing ? i18n::t("Pause") : goBack.isEmpty() ? i18n::t("Play") : goBack);
    m_play->setAccessibleDescription(goBack);
    m_volumeButton->setAccessibleName(m_view.muted ? i18n::t("Unmute the music") : i18n::t("Mute the music"));
    m_volumeButton->setToolTip(m_view.muted ? i18n::t("Music muted · click to unmute") : i18n::t("Volume %1% · click to mute, scroll to change").arg(m_view.volume));
    m_volume->setToolTip(i18n::t("Music volume: %1%").arg(m_view.volume));
    m_seek->setAccessibleDescription(i18n::t("%1 of %2").arg(formatDuration(m_view.positionMs), formatDuration(m_view.durationMs)));
    QString said;
    switch (m_view.mode) {
    case Mode::Loading:
        said = i18n::t("Loading %1").arg(m_view.title);
        break;
    case Mode::Playing:
        said = m_view.away.isEmpty() ? i18n::t("Playing %1 to the channel").arg(m_view.title) : i18n::t("Playing %1 on %2").arg(m_view.title, m_view.away);
        break;
    case Mode::Paused:
        said = away ? i18n::t("Paused: %1 on %2").arg(m_view.title, m_view.away) : i18n::t("Paused: %1").arg(m_view.title);
        break;
    case Mode::Notice:
        said = m_view.notice;
        break;
    }
    if (!m_view.problem.isEmpty() && m_view.mode != Mode::Notice)
        said += QStringLiteral(". ") + m_view.problem;
    if (said != m_announced) {
        m_announced = said;
        setAccessibleDescription(said);
    }
}

void MusicBar::relayout()
{
    m_geometry      = geometryFor(size(), font(), m_view);
    const bool note = m_view.mode == Mode::Notice;
    const auto place = [](QWidget* w, bool shown, const QRect& r) {
        if (shown)
            w->setGeometry(r);
        w->setVisible(shown);
    };
    place(m_play, !note, m_geometry.play);
    place(m_stop, !note, m_geometry.stop);
    place(m_close, note, m_geometry.close);
    place(m_seek, !note && m_geometry.showSeek, m_geometry.seek);
    place(m_volume, !note && m_geometry.showVolumeSlider, m_geometry.volumeSlider);
    place(m_volumeButton, !note && m_geometry.showVolumeButton, m_geometry.volumeButton);
}

void MusicBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // A wrapped notice is as high as its lines at this width: once the width is known, the layout asks again.
    if (m_view.mode == Mode::Notice && event->oldSize().width() != width() && heightForWidth(width()) != height())
        updateGeometry();
    relayout();
}

void MusicBar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
}

void MusicBar::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
}

QString MusicBar::titleTip() const
{
    const QString title = m_view.artist.isEmpty() ? m_view.title : i18n::t("%1 — %2").arg(m_view.title, m_view.artist);
    if (!m_view.away.isEmpty() && m_view.mode == Mode::Playing) // 2.3: on another server tab
        return i18n::t("%1\nPlaying on %2. Pause or stop it here; it plays on that server tab.").arg(title, m_view.away);
    if (!m_view.away.isEmpty()) // 2.3 live test: TeamSpeak sends only from the server tab in front
        return i18n::t("%1\nOn %2. TeamSpeak only sends from the server tab in front: Play takes you back there, Stop ends it.").arg(title, m_view.away);
    return title;
}

bool MusicBar::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto*         help = static_cast<QHelpEvent*>(event);
        const QPoint  pos  = help->pos();
        QString       tip;
        QRect         area;
        if (m_geometry.title.contains(pos)) {
            tip  = m_view.mode == Mode::Notice ? m_view.notice : titleTip();
            area = m_geometry.title;
        } else if (m_geometry.problem.contains(pos)) {
            tip  = m_view.problem;
            area = m_geometry.problem;
        } else if (m_geometry.badge.contains(pos) && m_view.mode != Mode::Notice) {
            tip  = m_view.live ? i18n::t("The channel hears the music") : i18n::t("The channel doesn't hear the music right now");
            area = m_geometry.badge;
        }
        if (tip.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), tip, this, area);
        return true;
    }
    return QWidget::event(event);
}

void MusicBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const Colors& c = m_colors;
    const QRectF  r(rect());

    // Attached to what is below it: rounded top corners, a hairline along the top (the reply bar's shape).
    QPainterPath bg;
    bg.moveTo(r.left(), r.bottom() + 1);
    bg.lineTo(r.left(), r.top() + kRadius);
    bg.quadTo(r.left(), r.top(), r.left() + kRadius, r.top());
    bg.lineTo(r.right() + 1 - kRadius, r.top());
    bg.quadTo(r.right() + 1, r.top(), r.right() + 1, r.top() + kRadius);
    bg.lineTo(r.right() + 1, r.bottom() + 1);
    bg.closeSubpath();
    p.fillPath(bg, c.surface);
    QPainterPath edge;
    edge.moveTo(r.left() + 0.5, r.bottom() + 1);
    edge.lineTo(r.left() + 0.5, r.top() + kRadius);
    edge.quadTo(r.left() + 0.5, r.top() + 0.5, r.left() + kRadius, r.top() + 0.5);
    edge.lineTo(r.right() + 1 - kRadius, r.top() + 0.5);
    edge.quadTo(r.right() + 0.5, r.top() + 0.5, r.right() + 0.5, r.top() + kRadius);
    edge.lineTo(r.right() + 0.5, r.bottom() + 1);
    p.setPen(QPen(c.line, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(edge);

    const QFont         tf = titleFont(font());
    const QFont         sf = smallFont(font());
    const QFontMetricsF tm(tf), sm(sf);
    const int           row = rowHeight(font());
    const auto baseline = [row](const QFontMetricsF& fm) { return std::round(row / 2.0 + (fm.ascent() - fm.descent()) / 2.0); };

    if (m_view.mode == Mode::Notice) {
        const bool error = m_view.noticeIcon == NoticeIcon::Error;
        drawMark(p, QRectF(m_geometry.badge), error, error ? c.error : c.accent);
        p.setFont(sf);
        p.setPen(c.text);
        const QStringList lines = noticeLines(font(), m_view.notice, m_geometry.title.width());
        if (lines.size() == 1 || height() <= row) {
            p.drawText(QPointF(m_geometry.title.left(), baseline(sm)), sm.elidedText(lines.join(QLatin1Char(' ')), Qt::ElideRight, m_geometry.title.width()));
        } else {
            qreal y = 10.0 + sm.ascent();
            for (const QString& line : lines) {
                p.drawText(QPointF(m_geometry.title.left(), std::round(y)), line);
                y += sm.lineSpacing();
            }
        }
        return;
    }

    // The badge (2.3): a music note in the paperclip menu's hue, grey while the channel doesn't hear it; a
    // ring while the file opens.
    const QRectF badge(m_geometry.badge);
    const bool   heard = m_view.live || m_view.mode != Mode::Playing;
    const QColor tone  = heard ? c.note : c.muted;
    p.setPen(Qt::NoPen);
    p.setBrush(heard ? c.noteTile : withAlpha(tone, m_dark ? 46 : 34));
    p.drawRoundedRect(badge, 6, 6);
    if (m_view.mode == Mode::Loading) {
        const QRectF ring = badge.adjusted(6, 6, -6, -6);
        p.setPen(QPen(withAlpha(tone, 90), 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ring);
        p.setPen(QPen(tone, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(ring, 90 * 16, -110 * 16);
    } else {
        drawBeamedNotes(p, badge.adjusted(3, 3, -3, -3), tone);
    }

    // The title, then the artist in the muted colour (the artist is cut first).
    const qreal titleRight = m_geometry.title.right() + 1;
    qreal       x          = m_geometry.title.left();
    QString     title      = m_view.title;
    QString     subtitle   = m_view.mode == Mode::Loading && m_view.artist.isEmpty() ? i18n::t("Loading…") : m_view.artist;
    if (!m_view.away.isEmpty())
        subtitle = i18n::t("on %1").arg(m_view.away); // 2.3: where it plays (another server tab)
    QString     artist     = subtitle.isEmpty() ? QString() : QStringLiteral("  ·  ") + subtitle;
    const qreal available  = titleRight - x;
    if (tm.horizontalAdvance(title) >= available - sm.horizontalAdvance(QStringLiteral("  ·  W…"))) {
        artist.clear();
        title = tm.elidedText(title, Qt::ElideRight, available);
    } else if (!artist.isEmpty()) {
        artist = sm.elidedText(artist, Qt::ElideRight, available - tm.horizontalAdvance(title));
    }
    p.setFont(tf);
    p.setPen(c.text);
    p.drawText(QPointF(x, baseline(tm)), title);
    x += tm.horizontalAdvance(title);
    if (!artist.isEmpty()) {
        p.setFont(sf);
        p.setPen(c.muted);
        p.drawText(QPointF(x, baseline(tm)), artist);
    }

    // The times.
    p.setFont(sf);
    p.setPen(c.muted);
    if (m_geometry.showElapsed) {
        const QString elapsed = m_view.mode == Mode::Loading ? QString() : formatDuration(shownPositionMs());
        p.drawText(QRectF(m_geometry.elapsed), Qt::AlignRight | Qt::AlignVCenter, elapsed);
    }
    if (m_geometry.showTotal)
        p.drawText(QRectF(m_geometry.total), Qt::AlignLeft | Qt::AlignVCenter, formatDuration(m_view.durationMs));

    // The problem line.
    if (!m_geometry.problem.isEmpty()) {
        const bool   warning = m_view.problemKind == Problem::Warning;
        const QColor ink     = warning ? c.error : c.muted;
        const QRectF line(m_geometry.problem);
        drawMark(p, QRectF(line.left() + 6, line.center().y() - 6, 12, 12), warning, ink);
        p.setPen(ink);
        const qreal tx = line.left() + 6 + 12 + 6;
        p.drawText(QPointF(tx, std::round(line.center().y() + (sm.ascent() - sm.descent()) / 2.0)),
                   sm.elidedText(m_view.problem, Qt::ElideRight, qMax(0.0, line.right() - tx)));
    }
}

QColor MusicBar::menuHue()
{
    return QColor(0xe6, 0x49, 0x80); // pink: 3.7:1 on white and on #2b2d31, like the attach menu's other hues
}

QIcon MusicBar::menuIcon()
{
    // Pixmaps only (no icon engine of this DLL inside: a copy that outlives the plugin is harmless), at the
    // attach menu's design size of 24 logical px and its usual scales.
    QIcon        icon;
    const QColor color = menuHue();
    for (const int size : {24, 30, 36, 48, 72}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        drawBeamedNotes(p, QRectF(0, 0, size, size), color);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
