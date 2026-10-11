#include "attachbutton.h"

#include <QAbstractScrollArea>
#include <QPainter>
#include <QTextEdit>

#include "attachart.h"
#include "attachmenu.h"
#include "attachpopup.h"
#include "i18n.h"

namespace {

QString objectNameText()
{
    return QString::fromLatin1("tsmediaAttachButton"); // fromLatin1: it lives among TeamSpeak's widgets
}

// The microphone's greys (micbutton.cpp), so the input's icons read as one set.
QColor idleColor(bool dark)
{
    return dark ? QColor(0xa7, 0xab, 0xb3) : QColor(0x85, 0x89, 0x91);
}
QColor hoverColor(bool dark)
{
    return dark ? QColor(0xf2, 0xf3, 0xf5) : QColor(0x31, 0x33, 0x38);
}
QColor accentColor(bool dark)
{
    return dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x58, 0x65, 0xf2);
}

} // namespace

AttachButton* AttachButton::of(const QWidget* input)
{
    return input ? input->findChild<AttachButton*>(objectNameText(), Qt::FindDirectChildrenOnly) : nullptr;
}

AttachButton* AttachButton::placeIn(QTextEdit* input, const QRect& strip)
{
    if (!input || strip.isEmpty())
        return nullptr;
    AttachButton* button = of(input);
    if (!button)
        button = new AttachButton(input);
    button->place(strip);
    button->show();
    button->raise();
    return button;
}

void AttachButton::removeFrom(QTextEdit* input)
{
    delete of(input);
}

AttachButton::AttachButton(QWidget* input)
    : QAbstractButton(input)
{
    setObjectName(objectNameText());
    setFocusPolicy(Qt::NoFocus); // typing stays in the input
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    // Copies made here (heap strings): TeamSpeak's tool tip may still show one after we are gone.
    const QString tip  = i18n::t("Attach");
    const QString name = i18n::t("Attach a file, a poll or live music");
    setToolTip(QString(tip.constData(), tip.size()));
    setAccessibleName(QString(name.constData(), name.size()));
    connect(this, &QAbstractButton::clicked, this, &AttachButton::openMenu);
}

AttachButton::~AttachButton()
{
    if (m_popup)
        delete m_popup.data();
}

void AttachButton::place(const QRect& strip)
{
    setGeometry(strip);
    // Centred on a one-line input, at the bottom of a taller one (like the microphone).
    m_icon = strip.height() <= 44 ? QPoint((strip.width() - kIcon) / 2, (strip.height() - kIcon) / 2)
                                  : QPoint((strip.width() - kIcon) / 2, strip.height() - kIcon - 3);
    update();
}

bool AttachButton::hitButton(const QPoint& pos) const
{
    return iconRect().adjusted(-3, -3, 3, 3).contains(pos);
}

void AttachButton::enterEvent(QEvent* event)
{
    QAbstractButton::enterEvent(event);
    update();
}

void AttachButton::leaveEvent(QEvent* event)
{
    QAbstractButton::leaveEvent(event);
    update();
}

bool AttachButton::dark() const
{
    const QWidget* w   = parentWidget() ? parentWidget() : this;
    const QPalette pal = w->palette();
    return pal.color(QPalette::Base).lightness() < 128 || pal.color(QPalette::Text).lightness() > 170;
}

void AttachButton::openMenu()
{
    if (m_popup) { // a click while it is open (the keyboard, a test): closes it
        m_popup->close();
        return;
    }
    const QList<attachmenu::Entry> entries = attachmenu::entries();
    if (entries.isEmpty())
        return;
    QColor base;
    if (auto* area = qobject_cast<QAbstractScrollArea*>(parentWidget()))
        base = area->viewport()->palette().color(QPalette::Base);
    auto* popup = new AttachPopup(entries, dark(), base, this);
    m_popup     = popup;
    connect(popup, &QObject::destroyed, this, [this] { update(); });
    popup->openAt(QRect(mapToGlobal(iconRect().topLeft()), iconRect().size()).adjusted(-3, -3, 3, 3));
    update();
}

void AttachButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // A native input paints only its viewport: the strip gets the same (like the microphone).
    auto* area = qobject_cast<QAbstractScrollArea*>(parentWidget());
    if (area && area->viewport()->autoFillBackground())
        p.fillRect(rect(), area->viewport()->palette().brush(area->viewport()->backgroundRole()));
    Look look;
    look.hover   = underMouse();
    look.pressed = isDown();
    look.open    = menuOpen();
    look.dark    = dark();
    paintIcon(p, QRectF(iconRect()), look);
}

void AttachButton::paintIcon(QPainter& p, const QRectF& box, const Look& look)
{
    // A little larger under the pointer, back on press (as the microphone and the emoji face).
    const qreal size  = look.pressed ? 21.0 : look.hover ? 23.0 : 21.0;
    QColor      color = idleColor(look.dark);
    if (look.pressed || look.open)
        color = accentColor(look.dark);
    else if (look.hover)
        color = hoverColor(look.dark);
    const QPointF c = box.center();
    attachart::draw(p, QRectF(c.x() - size / 2.0, c.y() - size / 2.0, size, size), attachart::Glyph::Paperclip, color);
}
