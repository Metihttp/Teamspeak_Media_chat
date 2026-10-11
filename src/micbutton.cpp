#include "micbutton.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTextEdit>
#include <QTimer>
#include <QVector>

#include <cmath>

#include "i18n.h"
#include "uiutil.h"

namespace {

constexpr int    kPulseTickMs   = 40;   // 25 frames a second, only while recording and shown
constexpr qint64 kPulsePeriodMs = 1400; // one breath

// What every button shows, and where the gestures go (VoiceController). Plain data: nothing here is
// handed to TeamSpeak's widgets except copies of the strings, which go with our buttons.
struct Shared {
    MicButton::State    state;
    QPointer<QObject>   receiver;
    MicButton::Handler  handler;
    QVector<MicButton*> buttons;
};

Shared& shared()
{
    static Shared s;
    return s;
}

QString objectNameText()
{
    return QString::fromLatin1("tsmediaMicButton"); // fromLatin1: it lives among TeamSpeak's widgets
}

// The colours, from the 2.1 tokens (the recorder window's red and accent). Icons need 3:1 on the input.
struct Palette {
    QColor idle;    // the microphone at rest (grey like the input's other icons)
    QColor hover;   // under the pointer
    QColor accent;  // pressed, and Busy
    QColor record;  // the recording disc
    QColor recordHover;
    QColor recordPressed;
    QColor canceled; // Holding in the cancel zone: the disc goes grey (it won't be sent)
};

Palette paletteFor(bool dark)
{
    Palette c;
    if (dark) {
        c.idle          = QColor(0xa7, 0xab, 0xb3);
        c.hover         = QColor(0xf2, 0xf3, 0xf5);
        c.accent        = QColor(0x94, 0x9c, 0xf7);
        c.record        = QColor(0xf2, 0x3f, 0x43);
        c.recordHover   = QColor(0xf5, 0x5c, 0x60);
        c.recordPressed = QColor(0xd6, 0x2f, 0x33);
        c.canceled      = QColor(0x80, 0x84, 0x8e);
    } else {
        c.idle          = QColor(0x85, 0x89, 0x91); // 3.5:1 on white, close to the emoji button's grey
        c.hover         = QColor(0x31, 0x33, 0x38);
        c.accent        = QColor(0x58, 0x65, 0xf2);
        c.record        = QColor(0xda, 0x37, 0x3c);
        c.recordHover   = QColor(0xc4, 0x28, 0x2d);
        c.recordPressed = QColor(0xa1, 0x22, 0x26);
        c.canceled      = QColor(0x6d, 0x6f, 0x78);
    }
    return c;
}

// A microphone in a square box: a filled capsule, its holder (a U), the stem and the foot, as one shape
// (filled once, so a translucent colour has no darker overlaps).
void drawMicrophone(QPainter& p, const QRectF& box, const QColor& color)
{
    // Built in a 2000-unit box and scaled down: the union flattens curves with a fixed tolerance, which
    // would make the capsule visibly polygonal at 20 units.
    static const QPainterPath shape = [] {
        const qreal  u = 100.0, x = 1000.0, top = 0.0;
        QPainterPath capsule;
        const qreal  width = 6.4 * u;
        capsule.addRoundedRect(QRectF(x - width / 2.0, top + 1.2 * u, width, 10.6 * u), width / 2.0, width / 2.0);
        QPainterPath lines;
        const QRectF arc(x - 6.0 * u, top + 3.0 * u, 12.0 * u, 11.6 * u);
        lines.arcMoveTo(arc, 180.0);
        lines.arcTo(arc, 180.0, 180.0);
        lines.moveTo(x, arc.bottom());
        lines.lineTo(x, top + 17.6 * u);
        lines.moveTo(x - 3.6 * u, top + 17.6 * u);
        lines.lineTo(x + 3.6 * u, top + 17.6 * u);
        QPainterPathStroker stroker;
        stroker.setWidth(1.7 * u);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        return capsule.united(stroker.createStroke(lines)).simplified();
    }();
    const qreal scale = box.width() / 2000.0;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.translate(box.topLeft());
    p.scale(scale, scale);
    p.drawPath(shape);
    p.restore();
}

// A soft ring breathing out of a disc: the microphone is open.
void drawPulse(QPainter& p, const QRectF& box, const QPointF& center, qreal radius, qreal phase, const QColor& color)
{
    const qreal room = qMin(box.width(), box.height()) / 2.0 + 2.5;
    QColor      ring = color;
    ring.setAlphaF(0.42 * (1.0 - phase));
    p.setPen(Qt::NoPen);
    p.setBrush(ring);
    const qreal r = qMin(room, radius + 4.0 * phase);
    p.drawEllipse(center, r, r);
}

} // namespace

// ---- every button ---------------------------------------------------------------------------------------

void MicButton::setSharedState(const State& state)
{
    Shared& s = shared();
    if (s.state == state)
        return;
    s.state = state;
    for (MicButton* button : qAsConst(s.buttons))
        button->setState(state);
}

const MicButton::State& MicButton::sharedState()
{
    return shared().state;
}

bool MicButton::anyVisible()
{
    for (const MicButton* button : qAsConst(shared().buttons)) {
        if (button->isVisible())
            return true;
    }
    return false;
}

void MicButton::setHandler(QObject* receiver, const Handler& handler)
{
    Shared& s  = shared();
    s.receiver = receiver;
    s.handler  = receiver ? handler : Handler();
}

void MicButton::clearHandler(QObject* receiver)
{
    Shared& s = shared();
    if (s.receiver && s.receiver.data() != receiver)
        return;
    s.receiver = nullptr;
    s.handler  = Handler();
}

void MicButton::abandonHolds()
{
    for (MicButton* button : qAsConst(shared().buttons))
        button->abandonHold();
}

// ---- EmojiInput's slot ------------------------------------------------------------------------------------

MicButton* MicButton::of(const QWidget* input)
{
    return input ? input->findChild<MicButton*>(objectNameText(), Qt::FindDirectChildrenOnly) : nullptr;
}

MicButton* MicButton::placeIn(QTextEdit* input, const QRect& strip)
{
    if (!input)
        return nullptr;
    MicButton* button = of(input);
    if (!button)
        button = new MicButton(input);
    button->place(strip);
    button->show();
    button->raise();
    return button;
}

void MicButton::removeFrom(QTextEdit* input)
{
    delete of(input);
}

// ---- the button -------------------------------------------------------------------------------------------

MicButton::MicButton(QWidget* input)
    : QAbstractButton(input)
{
    setObjectName(objectNameText());
    setFocusPolicy(Qt::NoFocus); // typing stays in the input
    setAttribute(Qt::WA_Hover);
    m_pulse = new QTimer(this);
    m_pulse->setInterval(kPulseTickMs);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        if (isVisible())
            update();
    });
    connect(this, &QAbstractButton::clicked, this, [] { // a press that wasn't a hold
        Shared& s = shared();
        if (s.receiver && s.handler.clicked)
            s.handler.clicked(s.receiver.data());
    });
    shared().buttons.append(this);
    setState(shared().state); // the texts are still empty: apply() sets everything
}

MicButton::~MicButton()
{
    stopTracking(); // our filter leaves the application with us
    shared().buttons.removeAll(this);
}

void MicButton::setState(const State& state)
{
    if (state == m_state && !accessibleName().isEmpty())
        return;
    m_state = state;
    apply();
}

void MicButton::apply()
{
    QString tip;
    QString name;
    QString description;
    switch (m_state.mode) {
    case Mode::Ready:
        tip         = i18n::t("Hold to record a voice message");
        name        = i18n::t("Record a voice message");
        description = i18n::t("Press and hold to record, release to send. Move the pointer away from the button before you release to cancel. "
                              "With the keyboard: Record voice message in the Plugins menu.");
        break;
    case Mode::Unavailable:
        tip         = m_state.reason.isEmpty() ? i18n::t("Voice messages can't be recorded here.") : m_state.reason;
        name        = i18n::t("Record a voice message");
        description = tip;
        break;
    case Mode::Holding: // no tool tip: the strip above the input says what to do
        name        = i18n::t("Recording a voice message");
        description = m_state.cancel ? i18n::t("Release to cancel.") : i18n::t("Release to send. Move away to cancel.");
        break;
    case Mode::Recording:
        tip         = i18n::t("Stop and send the voice message");
        name        = tip;
        description = i18n::t("Recording");
        break;
    case Mode::Busy:
        tip  = m_state.reason.isEmpty() ? i18n::t("Show the voice message window") : m_state.reason;
        name = tip;
        break;
    }
    // Copies made here (heap strings): TeamSpeak's tool tip may still show one after we are gone.
    if (toolTip() != tip)
        setToolTip(QString(tip.constData(), tip.size()));
    if (accessibleName() != name)
        setAccessibleName(QString(name.constData(), name.size()));
    if (accessibleDescription() != description)
        setAccessibleDescription(QString(description.constData(), description.size()));
    const bool usable = m_state.mode != Mode::Unavailable;
    setEnabled(usable);
    setCursor(usable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    updatePulse();
    update();
}

bool MicButton::pulsing() const
{
    return m_pulse->isActive();
}

void MicButton::updatePulse()
{
    const bool open   = m_state.mode == Mode::Recording || (m_state.mode == Mode::Holding && !m_state.cancel);
    const bool wanted = open && isVisible() && ui::animationsEnabled();
    if (wanted == m_pulse->isActive())
        return;
    if (wanted) {
        m_pulseClock.start();
        m_pulse->start();
    } else {
        m_pulse->stop();
    }
}

void MicButton::place(const QRect& strip)
{
    setGeometry(strip);
    // Centred on a one-line input, at the bottom of a taller one (like the emoji button).
    m_icon = strip.height() <= 44 ? QPoint((strip.width() - kIcon) / 2, (strip.height() - kIcon) / 2)
                                  : QPoint((strip.width() - kIcon) / 2, strip.height() - kIcon - 3);
    update();
}

bool MicButton::hitButton(const QPoint& pos) const
{
    return iconRect().adjusted(-3, -3, 3, 3).contains(pos);
}

// ---- hold to record -----------------------------------------------------------------------------------

void MicButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractButton::mousePressEvent(event);
        return;
    }
    if (m_hold) { // another press while one is held (a second mouse): nothing
        event->accept();
        return;
    }
    // A new press: the one before is over, even if its release never came here (let go in another app).
    m_swallow = false;
    Shared&    s      = shared();
    const bool second = event->type() == QEvent::MouseButtonDblClick; // a double click's second press: never a hold
    if (!second && isEnabled() && hitButton(event->pos()) && s.receiver && s.handler.pressed) {
        m_center = mapToGlobal(iconRect().center());
        // The handler may refresh the state of every button: it must see this hold first.
        m_hold       = true;
        m_cancelZone = false;
        if (s.handler.pressed(s.receiver.data(), this)) {
            if (m_hold) // nothing ended it meanwhile
                beginHold(event->globalPos());
            event->accept();
            return;
        }
        m_hold = false;
        if (!isEnabled()) {
            // The handler found that this chat can't take one and greyed the buttons out: the press ends
            // here (an ordinary press would leave a disabled button down, its release never coming).
            event->accept();
            return;
        }
    }
    QAbstractButton::mousePressEvent(event); // not a hold: an ordinary button
}

void MicButton::mouseDoubleClickEvent(QMouseEvent* event)
{
    // The second press of a double click: never a new hold (the first one was a tap, or is still going),
    // and no second click either while a hold would start.
    if (event->button() == Qt::LeftButton && (m_hold || m_state.mode == Mode::Ready || m_state.mode == Mode::Holding)) {
        if (!m_hold)
            m_swallow = true;
        event->accept();
        return;
    }
    QAbstractButton::mouseDoubleClickEvent(event); // an ordinary press (QWidget's calls mousePressEvent)
}

void MicButton::beginHold(const QPoint& globalPos)
{
    m_hold       = true;
    m_swallow    = false;
    m_cancelZone = zoneAt(globalPos);
    setDown(true);
    if (!m_filtering && qApp) {
        qApp->installEventFilter(this); // Esc goes to whatever has the focus (the input, usually)
        m_filtering = true;
    }
    update();
}

void MicButton::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_hold) {
        if (!m_swallow)
            QAbstractButton::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const bool zone = zoneAt(event->globalPos());
    if (zone == m_cancelZone)
        return;
    m_cancelZone = zone;
    update();
    Shared& s = shared();
    if (s.receiver && s.handler.zoneChanged)
        s.handler.zoneChanged(s.receiver.data(), zone);
}

void MicButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractButton::mouseReleaseEvent(event);
        return;
    }
    if (m_swallow) { // the press ended already (Esc, lost, abandoned, a double click's second press)
        m_swallow = false;
        setDown(false);
        event->accept();
        return;
    }
    if (!m_hold) {
        QAbstractButton::mouseReleaseEvent(event);
        return;
    }
    event->accept();
    m_cancelZone = zoneAt(event->globalPos()); // the release's own place (no move may have come first)
    endHold(m_cancelZone ? Release::Cancel : Release::Send);
}

bool MicButton::zoneAt(const QPoint& globalPos) const
{
    const QPoint d    = globalPos - m_center;
    const qreal  away = std::hypot(static_cast<qreal>(d.x()), static_cast<qreal>(d.y()));
    return m_cancelZone ? away > kRearmPx : away > kCancelPx;
}

void MicButton::endHold(Release how)
{
    if (!m_hold)
        return;
    m_hold = false;
    // A release still has to come unless this is it.
    m_swallow = how == Release::Escape || how == Release::Lost;
    stopTracking();
    Shared& s = shared();
    if (s.receiver && s.handler.released)
        s.handler.released(s.receiver.data(), how);
}

void MicButton::abandonHold()
{
    if (!m_hold) // over already (an ordinary press going on now stays as it is)
        return;
    m_hold    = false;
    m_swallow = true;
    stopTracking();
}

void MicButton::stopTracking()
{
    if (m_filtering && qApp)
        qApp->removeEventFilter(this);
    m_filtering = false;
    if (isDown())
        setDown(false);
    m_cancelZone = false;
    update();
}

bool MicButton::eventFilter(QObject* watched, QEvent* event)
{
    if (m_hold && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept(); // ShortcutOverride: none of TeamSpeak's shortcuts takes it
        if (event->type() == QEvent::KeyPress)
            endHold(Release::Escape);
        return true;
    }
    return QAbstractButton::eventFilter(watched, event);
}

bool MicButton::event(QEvent* event)
{
    // TeamSpeak's window lost the activation (Alt+Tab, another app, a window popping up): Windows takes the
    // mouse away, so the release may never come here.
    if (m_hold && event->type() == QEvent::WindowDeactivate)
        endHold(Release::Lost);
    // Disabled while held (the input or a parent of it): a disabled button gets no release.
    if (m_hold && event->type() == QEvent::EnabledChange && !isEnabled())
        endHold(Release::Lost);
    return QAbstractButton::event(event);
}

void MicButton::enterEvent(QEvent* event)
{
    QAbstractButton::enterEvent(event);
    Shared& s = shared();
    if (!m_hold && s.receiver && s.handler.hovered)
        s.handler.hovered(s.receiver.data()); // may set a new state (and tool tip) before Qt shows it
    update();
}

void MicButton::leaveEvent(QEvent* event)
{
    QAbstractButton::leaveEvent(event);
    update();
}

void MicButton::showEvent(QShowEvent* event)
{
    QAbstractButton::showEvent(event);
    updatePulse();
}

void MicButton::hideEvent(QHideEvent* event)
{
    QAbstractButton::hideEvent(event);
    if (m_hold)
        endHold(Release::Lost); // another server tab, minimized, or the input went: no release comes here
    updatePulse();
}

bool MicButton::dark() const
{
    const QWidget* w   = parentWidget() ? parentWidget() : this;
    const QPalette pal = w->palette();
    return pal.color(QPalette::Base).lightness() < 128 || pal.color(QPalette::Text).lightness() > 170;
}

void MicButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // A native input paints only its viewport: the strip gets the same (like the emoji button). With
    // TeamSpeak's style sheets the input's background already covers it.
    auto* area = qobject_cast<QAbstractScrollArea*>(parentWidget());
    if (area && area->viewport()->autoFillBackground())
        p.fillRect(rect(), area->viewport()->palette().brush(area->viewport()->backgroundRole()));
    Look look;
    look.hover   = isEnabled() && underMouse();
    look.pressed = isEnabled() && isDown();
    look.dark    = dark();
    look.cancel  = m_state.cancel;
    if (m_pulse->isActive())
        look.pulse = static_cast<qreal>(m_pulseClock.elapsed() % kPulsePeriodMs) / static_cast<qreal>(kPulsePeriodMs);
    paintIcon(p, QRectF(iconRect()), m_state.mode, look);
}

void MicButton::paintIcon(QPainter& p, const QRectF& box, Mode mode, const Look& look)
{
    const Palette c = paletteFor(look.dark);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF center = box.center();
    switch (mode) {
    case Mode::Ready:
    case Mode::Busy: {
        // A little larger under the pointer, back on press (as the emoji button).
        const qreal  size  = look.pressed ? 20.0 : look.hover ? 22.0 : 20.0;
        QColor       color = mode == Mode::Busy ? c.accent : c.idle;
        if (look.pressed)
            color = c.accent;
        else if (look.hover && mode == Mode::Ready)
            color = c.hover;
        drawMicrophone(p, QRectF(center.x() - size / 2.0, center.y() - size / 2.0, size, size), color);
        break;
    }
    case Mode::Unavailable: {
        QColor dimmed = c.idle; // disabled: reduced, never hidden (the tool tip says why)
        dimmed.setAlphaF(0.45);
        drawMicrophone(p, QRectF(center.x() - 10.0, center.y() - 10.0, 20.0, 20.0), dimmed);
        break;
    }
    case Mode::Holding: {
        // Held: the microphone, white on the red disc; grey in the cancel zone (it won't be sent).
        const qreal radius = 10.5;
        if (look.pulse >= 0.0 && !look.cancel)
            drawPulse(p, box, center, radius, look.pulse, c.record);
        p.setPen(Qt::NoPen);
        p.setBrush(look.cancel ? c.canceled : c.record);
        p.drawEllipse(center, radius, radius);
        const qreal size = 13.0;
        drawMicrophone(p, QRectF(center.x() - size / 2.0, center.y() - size / 2.0, size, size), Qt::white);
        break;
    }
    case Mode::Recording: {
        const qreal radius = 10.0;
        if (look.pulse >= 0.0)
            drawPulse(p, box, center, radius, look.pulse, c.record);
        p.setPen(Qt::NoPen);
        p.setBrush(look.pressed ? c.recordPressed : look.hover ? c.recordHover : c.record);
        p.drawEllipse(center, radius, radius);
        const qreal stop = 7.0;
        p.setBrush(Qt::white);
        p.drawRoundedRect(QRectF(center.x() - stop / 2.0, center.y() - stop / 2.0, stop, stop), 1.6, 1.6);
        break;
    }
    }
    p.restore();
}
