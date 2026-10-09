#include "reactionpicker.h"

#include <QAbstractButton>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>

#include "i18n.h"
#include "peerprotocol.h"
#include "reactionart.h"
#include "uiutil.h"

namespace {

constexpr int kPadding = 6;
constexpr int kButton  = 36;
constexpr int kGap     = 4;
constexpr int kIcon    = 24;

const QColor kAccent(0x58, 0x65, 0xf2);
const QColor kAccentOnDark(0x79, 0x84, 0xf5); // 3:1 against the dark popup too

class ReactionButton : public QAbstractButton
{
  public:
    ReactionButton(int reaction, bool dark, bool* keyboardUsed, QWidget* parent)
        : QAbstractButton(parent)
        , m_reaction(reaction)
        , m_dark(dark)
        , m_keyboardUsed(keyboardUsed)
    {
        setCheckable(true); // "checked" = one of your reactions, also for screen readers
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(kButton, kButton);
        const QString name = rx::reactionName(reaction);
        setAccessibleName(name);
        setToolTip(name);
        setAttribute(Qt::WA_Hover);
    }

    int reaction() const { return m_reaction; }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor       fill(Qt::transparent);
        if (isDown())
            fill = QColor(128, 128, 128, 61);
        else if (underMouse())
            fill = QColor(128, 128, 128, 41);
        const QColor accent = m_dark ? kAccentOnDark : kAccent;
        if (isChecked()) {
            p.setPen(QPen(accent, 1.0));
            p.setBrush(isDown() || underMouse() ? QColor(88, 101, 242, 77) : QColor(88, 101, 242, 46));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
        }
        p.drawRoundedRect(r, 6, 6);
        if (hasFocus() && *m_keyboardUsed) {
            p.setPen(QPen(accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 5, 5);
        }
        const qreal offset = isDown() ? 1.0 : 0.0;
        rx::drawReaction(p, QRectF((width() - kIcon) / 2.0, (height() - kIcon) / 2.0 + offset, kIcon, kIcon), m_reaction);
    }

    // "Checked" shows a reaction you already have; a click picks it (the picker closes) instead of
    // flipping the look first.
    void nextCheckState() override {}

  private:
    int   m_reaction;
    bool  m_dark;
    bool* m_keyboardUsed;
};

bool g_keyboardUsed = false; // per open picker (only one at a time)

} // namespace

ReactionPicker::ReactionPicker(bool dark, const QColor& base, int ownMask, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
    , m_dark(dark)
    , m_base(base)
{
    setObjectName(QString::fromLatin1("tsmediaReactionPicker"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setAccessibleName(i18n::t("Reactions"));
    setLayoutDirection(Qt::LeftToRight);
    setFixedSize(fixedSize());
    g_keyboardUsed = false;
    for (int i = 0; i < proto::kReactionCount; ++i) {
        auto* button = new ReactionButton(i, dark, &g_keyboardUsed, this);
        button->move(kPadding + i * (kButton + kGap), kPadding);
        button->setChecked(ownMask & (1 << i));
        connect(button, &QAbstractButton::clicked, this, [this, i] { choose(i); });
        m_buttons.append(button);
    }
}

QSize ReactionPicker::fixedSize()
{
    return QSize(2 * kPadding + proto::kReactionCount * kButton + (proto::kReactionCount - 1) * kGap, 2 * kPadding + kButton);
}

void ReactionPicker::openAt(const QRect& anchor)
{
    const QSize size = fixedSize();
    QPoint      at(anchor.right() - size.width() + 1, anchor.bottom() + 4);
    QScreen*    screen = QGuiApplication::screenAt(anchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (at.y() + size.height() > area.bottom())
            at.setY(anchor.top() - 4 - size.height()); // no room below: above it
        at.setX(qBound(area.left(), at.x(), area.right() - size.width() + 1));
        at.setY(qBound(area.top(), at.y(), area.bottom() - size.height() + 1));
    }
    move(at);
    show();
    raise();
    activateWindow();
    if (!m_buttons.isEmpty())
        m_buttons.first()->setFocus(Qt::PopupFocusReason);
}

void ReactionPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor background = m_dark ? QColor(0x2b, 0x2d, 0x31) : QColor(0xff, 0xff, 0xff);
    QColor border     = m_dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0xe3, 0xe5, 0xe8);
    if (m_dark && m_base.isValid() && m_base.lightness() < 128)
        background = ui::flatten(QColor(255, 255, 255, 8), m_base); // a step above the chat
    p.setPen(QPen(border, 1.0));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void ReactionPicker::keyPressEvent(QKeyEvent* event)
{
    int current = -1;
    for (int i = 0; i < m_buttons.size(); ++i) {
        if (m_buttons.at(i)->hasFocus())
            current = i;
    }
    const bool firstKey = !g_keyboardUsed;
    g_keyboardUsed      = true;
    if (firstKey && current >= 0)
        m_buttons.at(current)->update(); // the focus ring appears with the first key
    const int key = event->key();
    if (key >= Qt::Key_1 && key < Qt::Key_1 + proto::kReactionCount) {
        choose(key - Qt::Key_1);
        return;
    }
    switch (key) {
    case Qt::Key_Left:
        moveFocus(current <= 0 ? m_buttons.size() - 1 : current - 1);
        return;
    case Qt::Key_Right:
        moveFocus(current < 0 || current + 1 >= m_buttons.size() ? 0 : current + 1);
        return;
    case Qt::Key_Home:
        moveFocus(0);
        return;
    case Qt::Key_End:
        moveFocus(m_buttons.size() - 1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (current >= 0)
            choose(current);
        return;
    case Qt::Key_Escape:
        close();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void ReactionPicker::moveFocus(int index)
{
    if (index >= 0 && index < m_buttons.size())
        m_buttons.at(index)->setFocus(Qt::TabFocusReason);
}

void ReactionPicker::choose(int reaction)
{
    emit picked(reaction);
    close();
}
