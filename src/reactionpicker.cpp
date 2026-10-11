#include "reactionpicker.h"

#include <QAbstractButton>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
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
    // reaction: an emoji id, or -1 for "+" (more reactions).
    ReactionButton(int reaction, bool dark, bool* keyboardUsed, QWidget* parent)
        : QAbstractButton(parent)
        , m_reaction(reaction)
        , m_dark(dark)
        , m_keyboardUsed(keyboardUsed)
    {
        setCheckable(reaction >= 0); // "checked" = one of your reactions, also for screen readers
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(kButton, kButton);
        const QString name = reaction >= 0 ? rx::reactionName(reaction) : i18n::t("More reactions");
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
        const qreal  offset = isDown() ? 1.0 : 0.0;
        const QRectF icon((width() - kIcon) / 2.0, (height() - kIcon) / 2.0 + offset, kIcon, kIcon);
        if (m_reaction >= 0)
            rx::drawReaction(p, icon, m_reaction);
        else
            rx::drawAddReactionGlyph(p, icon.adjusted(3, 3, -3, -3), m_dark ? QColor(0xb5, 0xba, 0xc1) : QColor(0x4e, 0x50, 0x58));
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

ReactionPicker::ReactionPicker(bool dark, const QColor& base, const QVector<int>& quick, const QSet<int>& own, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
    , m_dark(dark)
    , m_base(base)
    , m_quick(quick.mid(0, 8))
{
    setObjectName(QString::fromLatin1("tsmediaReactionPicker"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setAccessibleName(i18n::t("Reactions"));
    setLayoutDirection(Qt::LeftToRight);
    setFixedSize(fixedSize(m_quick.size()));
    g_keyboardUsed = false;
    for (int i = 0; i <= m_quick.size(); ++i) {
        const int reaction = i < m_quick.size() ? m_quick.at(i) : -1;
        auto*     button   = new ReactionButton(reaction, dark, &g_keyboardUsed, this);
        // A thin gap before "+".
        button->move(kPadding + i * (kButton + kGap) + (i == m_quick.size() ? kGap : 0), kPadding);
        if (reaction >= 0)
            button->setChecked(own.contains(reaction));
        connect(button, &QAbstractButton::clicked, this, [this, i] { choose(i); });
        m_buttons.append(button);
    }
}

QSize ReactionPicker::fixedSize(int quickCount)
{
    const int buttons = quickCount + 1;
    return QSize(2 * kPadding + buttons * kButton + (buttons - 1) * kGap + kGap, 2 * kPadding + kButton);
}

QSize ReactionPicker::sizeForCount() const
{
    return fixedSize(m_quick.size());
}

void ReactionPicker::openAt(const QRect& anchor)
{
    const QSize size = this->size();
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
    // The divider before "+".
    if (m_buttons.size() > 1) {
        const qreal x = m_buttons.last()->x() - kGap - 0.5;
        p.setPen(QPen(m_dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 25), 1.0));
        p.drawLine(QPointF(x, kPadding + 8), QPointF(x, height() - kPadding - 8));
    }
}

void ReactionPicker::mousePressEvent(QMouseEvent* event)
{
    // A press outside closes the popup and Qt hands it on; on the button that opened it that would open it
    // again at once: a second click on that button closes it instead.
    if (!rect().contains(event->pos()) && m_opener.contains(event->globalPos()))
        setAttribute(Qt::WA_NoMouseReplay);
    QWidget::mousePressEvent(event);
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
    if (key >= Qt::Key_1 && key < Qt::Key_1 + m_quick.size()) {
        choose(key - Qt::Key_1);
        return;
    }
    switch (key) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        choose(m_quick.size());
        return;
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

void ReactionPicker::choose(int index)
{
    if (index >= 0 && index < m_quick.size())
        emit picked(m_quick.at(index));
    else if (index == m_quick.size())
        emit more();
    close();
}
