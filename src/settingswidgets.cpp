#include "settingswidgets.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionButton>

#include <cmath>

#include "i18n.h"
#include "settingstheme.h"

using settingstheme::mix;
using settingstheme::Tokens;

// ---- icons ------------------------------------------------------------------------------------------------------------
namespace settingsicons {

void draw(QPainter& p, Icon icon, const QRectF& r, const QColor& c)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.topLeft());
    p.scale(r.width() / 24.0, r.height() / 24.0);
    p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    switch (icon) {
    case Icon::Chat:
        path.addRoundedRect(QRectF(3, 4, 18, 13), 3, 3);
        p.drawPath(path);
        p.drawPolyline(QPolygonF({QPointF(7.5, 17), QPointF(7.5, 21), QPointF(12, 17)}));
        p.drawLine(QPointF(7.5, 9), QPointF(16.5, 9));
        p.drawLine(QPointF(7.5, 12.5), QPointF(13.5, 12.5));
        break;
    case Icon::Emoji:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawArc(QRectF(7.5, 8.5, 9, 8), 200 * 16, 140 * 16);
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(9, 10), 1.2, 1.2);
        p.drawEllipse(QPointF(15, 10), 1.2, 1.2);
        break;
    case Icon::Send:
        p.drawPolygon(QPolygonF({QPointF(21, 3), QPointF(14.5, 21), QPointF(11, 13), QPointF(3, 9.5)}));
        p.drawLine(QPointF(21, 3), QPointF(11, 13));
        break;
    case Icon::Receive:
        p.drawLine(QPointF(12, 3), QPointF(12, 14.5));
        p.drawPolyline(QPolygonF({QPointF(7.5, 10), QPointF(12, 14.5), QPointF(16.5, 10)}));
        p.drawPolyline(QPolygonF({QPointF(4, 14.5), QPointF(4, 20), QPointF(20, 20), QPointF(20, 14.5)}));
        break;
    case Icon::Link:
        p.translate(12, 12);
        p.rotate(-45);
        p.drawRoundedRect(QRectF(-9.5, -3.5, 10.5, 7), 3.5, 3.5);
        p.drawRoundedRect(QRectF(-1, -3.5, 10.5, 7), 3.5, 3.5);
        break;
    case Icon::Shield:
        path.moveTo(12, 21.5);
        path.cubicTo(12, 21.5, 19.5, 18, 19.5, 12);
        path.lineTo(19.5, 5.5);
        path.lineTo(12, 2.5);
        path.lineTo(4.5, 5.5);
        path.lineTo(4.5, 12);
        path.cubicTo(4.5, 18, 12, 21.5, 12, 21.5);
        p.drawPath(path);
        p.drawPolyline(QPolygonF({QPointF(8.8, 12), QPointF(11, 14.2), QPointF(15.4, 9.6)}));
        break;
    case Icon::Server:
        p.drawRoundedRect(QRectF(3.5, 3.5, 17, 7), 2, 2);
        p.drawRoundedRect(QRectF(3.5, 13.5, 17, 7), 2, 2);
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(7.5, 7), 1.2, 1.2);
        p.drawEllipse(QPointF(7.5, 17), 1.2, 1.2);
        break;
    case Icon::Info:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawLine(QPointF(12, 11), QPointF(12, 16.5));
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 7.6), 1.25, 1.25);
        break;
    case Icon::Minus:
        p.drawLine(QPointF(6, 12), QPointF(18, 12));
        break;
    case Icon::Plus:
        p.drawLine(QPointF(6, 12), QPointF(18, 12));
        p.drawLine(QPointF(12, 6), QPointF(12, 18));
        break;
    }
    p.restore();
}

} // namespace settingsicons

// ---- shared helpers ---------------------------------------------------------------------------------------------------
namespace settingswidgets {

QFont windowFont(const QWidget* w)
{
    const QWidget* top = w ? w->window() : nullptr;
    return top ? top->font() : (w ? w->font() : QFont());
}

void focusRing(QPainter& p, const QRectF& r, qreal radius, const Tokens& t)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(t.focus, t.px(2)));
    p.drawRoundedRect(r, radius, radius);
    p.restore();
}

bool showMnemonics(const QWidget* w)
{
    if (!w)
        return false;
    QStyleOption option;
    option.initFrom(w);
    return w->style()->styleHint(QStyle::SH_UnderlineShortcut, &option, w) != 0;
}

} // namespace settingswidgets

using settingswidgets::focusRing;
using settingswidgets::showMnemonics;
using settingswidgets::windowFont;

// ---- SwitchBox --------------------------------------------------------------------------------------------------------
SwitchBox::SwitchBox(const QString& text, QWidget* parent)
    : QCheckBox(text, parent)
{
    setAttribute(Qt::WA_Hover);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

QSize SwitchBox::trackSize(const QFont& font, qreal k)
{
    const int h = qMax(qRound(18 * k), qRound(QFontMetricsF(font).height() * 1.25));
    return QSize(2 * h, h);
}

void SwitchBox::setTrailingReserve(int px)
{
    if (px == m_reserve)
        return;
    m_reserve = px;
    updateGeometry();
    update();
}

int SwitchBox::textWidth(int width) const
{
    const Tokens t = settingstheme::tokensFor(this);
    return qMax(t.px(40), width - trackSize(windowFont(this), t.k).width() - t.px(16) - m_reserve - t.px(4));
}

QSize SwitchBox::sizeHint() const
{
    const Tokens       t = settingstheme::tokensFor(this);
    const QFont        f = windowFont(this);
    const QFontMetrics fm(f);
    const QSize        track = trackSize(f, t.k);
    QString            plain = text();
    plain.remove(QLatin1Char('&'));
    return QSize(fm.horizontalAdvance(plain) + t.px(16) + track.width() + m_reserve + t.px(4), qMax(track.height() + t.px(8), fm.height()));
}

QSize SwitchBox::minimumSizeHint() const
{
    const Tokens t     = settingstheme::tokensFor(this);
    const QSize  track = trackSize(windowFont(this), t.k);
    return QSize(track.width() + t.px(16) + m_reserve + t.px(120), track.height() + t.px(4));
}

int SwitchBox::heightForWidth(int width) const
{
    const Tokens       t = settingstheme::tokensFor(this);
    const QFont        f = windowFont(this);
    const QFontMetrics fm(f);
    const QRect        need = fm.boundingRect(QRect(0, 0, textWidth(width), 100000), Qt::TextWordWrap | Qt::TextShowMnemonic, text());
    return qMax(need.height(), trackSize(f, t.k).height() + t.px(8));
}

bool SwitchBox::hitButton(const QPoint& pos) const
{
    // The whole row's title and the switch; not the trailing control's place between them.
    const Tokens t     = settingstheme::tokensFor(this);
    const int    track = trackSize(windowFont(this), t.k).width();
    return rect().contains(pos) && (pos.x() < width() - m_reserve - track - t.px(16) - t.px(4) || pos.x() > width() - track - t.px(6));
}

void SwitchBox::paintEvent(QPaintEvent*)
{
    const Tokens t = settingstheme::tokensFor(this);
    QPainter     p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFont font = windowFont(this);
    p.setFont(font);
    const QFontMetrics fm(font);
    const QSize        ts    = trackSize(font, t.k);
    const bool         en    = isEnabled();
    const bool         on    = isChecked();
    const bool         hover = en && underMouse();
    const int          lineH = fm.height();
    // The switch sits centred on the row for a one-line title, on the first line of a wrapped one.
    const QRect  need    = fm.boundingRect(QRect(0, 0, textWidth(width()), 100000), Qt::TextWordWrap | Qt::TextShowMnemonic, text());
    const bool   oneLine = need.height() <= lineH + 1;
    const qreal  top     = oneLine ? (height() - ts.height()) / 2.0 : qMax<qreal>(0, (lineH - ts.height()) / 2.0);
    const QRectF tr(width() - ts.width() - t.px(4), top, ts.width(), ts.height());
    const qreal  r = tr.height() / 2.0;
    QColor       fill(Qt::transparent), edge, knob;
    if (!en) {
        fill = on ? t.disabled : QColor(Qt::transparent);
        edge = t.disabled;
        knob = on ? t.card : t.disabled;
    } else if (on) {
        fill = hover ? mix(t.accent, t.text, 0.12) : t.accent;
        edge = fill;
        knob = t.onAccent;
    } else {
        fill = hover ? mix(t.card, t.text, 0.07) : QColor(Qt::transparent);
        edge = t.control;
        knob = t.control;
    }
    p.setPen(QPen(edge, 1));
    p.setBrush(fill);
    p.drawRoundedRect(tr.adjusted(0.5, 0.5, -0.5, -0.5), r - 0.5, r - 0.5);
    const qreal kd = tr.height() * (hover ? 0.68 : 0.58);
    const qreal cx = on ? tr.right() - r : tr.left() + r;
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    p.drawEllipse(QPointF(cx, tr.center().y()), kd / 2, kd / 2);
    if (settingstheme::showsFocus(this))
        focusRing(p, tr.adjusted(-t.px(3), -t.px(3), t.px(3), t.px(3)), r + t.px(3), t);

    int flags = Qt::AlignLeft | Qt::TextWordWrap | (showMnemonics(this) ? Qt::TextShowMnemonic : Qt::TextHideMnemonic);
    flags |= oneLine ? Qt::AlignVCenter : Qt::AlignTop;
    p.setPen(en ? t.text : t.disabled);
    p.drawText(QRect(0, 0, textWidth(width()), height()), flags, text());
}

void SwitchBox::enterEvent(QEvent* event)
{
    update();
    QCheckBox::enterEvent(event);
}

void SwitchBox::leaveEvent(QEvent* event)
{
    update();
    QCheckBox::leaveEvent(event);
}

// ---- LayoutTile -------------------------------------------------------------------------------------------------------
LayoutTile::LayoutTile(Kind kind, const QString& text, QWidget* parent)
    : QRadioButton(text, parent)
    , m_kind(kind)
{
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize LayoutTile::sizeHint() const
{
    const Tokens       t = settingstheme::tokensFor(this);
    const QFontMetrics fm(windowFont(this));
    return QSize(t.px(170), t.px(64) + t.px(10) + t.px(8) + fm.height() * 3 + t.px(14));
}

QSize LayoutTile::minimumSizeHint() const
{
    return QSize(settingstheme::tokensFor(this).px(150), sizeHint().height());
}

bool LayoutTile::hitButton(const QPoint& pos) const
{
    return rect().contains(pos);
}

void LayoutTile::enterEvent(QEvent* event)
{
    update();
    QRadioButton::enterEvent(event);
}

void LayoutTile::leaveEvent(QEvent* event)
{
    update();
    QRadioButton::leaveEvent(event);
}

void LayoutTile::paintEvent(QPaintEvent*)
{
    const Tokens t = settingstheme::tokensFor(this);
    QPainter     p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool   on    = isChecked();
    const bool   hover = underMouse();
    const QRectF box   = QRectF(rect()).adjusted(t.px(1), t.px(1), -t.px(1), -t.px(1));
    const qreal  pen   = on ? t.px(2) : 1;
    p.setPen(QPen(on ? t.accent : hover ? t.control : t.border, pen));
    p.setBrush(t.card);
    p.drawRoundedRect(box.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2), t.px(8), t.px(8));

    // A schematic of the layout, in token colours.
    const QRectF art(box.left() + t.px(10), box.top() + t.px(10), box.width() - t.px(20), t.px(64));
    p.setPen(Qt::NoPen);
    p.setBrush(mix(t.card, t.text, t.dark ? 0.06 : 0.04));
    p.drawRoundedRect(art, t.px(6), t.px(6));
    const QColor ink     = mix(t.card, t.text, t.dark ? 0.38 : 0.30);
    const QColor faint   = mix(t.card, t.text, t.dark ? 0.22 : 0.16);
    const QColor names[] = {QColor(0xe0, 0x6c, 0x75), QColor(0x56, 0xb6, 0xc2), QColor(0xc6, 0x78, 0xdd)};
    const auto   bar     = [&](qreal x, qreal y, qreal w, qreal h, const QColor& c) {
        p.setBrush(c);
        p.drawRoundedRect(QRectF(x, y, w, h), h / 2, h / 2);
    };
    const qreal u  = t.k;
    const qreal x0 = art.left() + 9 * u, y0 = art.top() + 9 * u, w = art.width() - 18 * u;
    if (m_kind == Kind::Cozy) {
        for (int i = 0; i < 2; ++i) {
            const qreal y = y0 + i * 25 * u;
            p.setBrush(names[i]);
            p.drawEllipse(QRectF(x0, y, 16 * u, 16 * u));
            bar(x0 + 22 * u, y + 1 * u, w * 0.28, 5 * u, names[i]);
            bar(x0 + 22 * u, y + 9 * u, w * (i ? 0.55 : 0.7), 5 * u, ink);
            if (!i)
                bar(x0 + 22 * u, y + 17 * u, w * 0.4, 5 * u, faint);
        }
    } else if (m_kind == Kind::Compact) {
        for (int i = 0; i < 5; ++i) {
            const qreal y = y0 + i * 9.5 * u;
            bar(x0, y, 14 * u, 4.5 * u, faint);
            bar(x0 + 18 * u, y, 18 * u, 4.5 * u, names[i % 3]);
            bar(x0 + 40 * u, y, (w - 40 * u) * (0.9 - 0.13 * (i % 3)), 4.5 * u, ink);
        }
    } else {
        const QColor blue = t.dark ? QColor(0x6d, 0xa6, 0xf0) : QColor(0x1c, 0x52, 0xcc);
        for (int i = 0; i < 5; ++i) {
            const qreal y = y0 + i * 9.5 * u;
            bar(x0, y, 22 * u, 4.5 * u, faint);
            bar(x0 + 26 * u, y, 20 * u, 4.5 * u, blue);
            bar(x0 + 50 * u, y, (w - 50 * u) * (0.85 - 0.12 * (i % 3)), 4.5 * u, ink);
        }
    }
    if (on) { // the check badge: a shape, not only a colour
        const QPointF c(box.right() - t.px(14), box.top() + t.px(14));
        p.setPen(Qt::NoPen);
        p.setBrush(t.accent);
        p.drawEllipse(c, t.px(8), t.px(8));
        p.setPen(QPen(t.onAccent, 1.8 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({c + QPointF(-3.6 * u, 0.2 * u), c + QPointF(-1 * u, 2.8 * u), c + QPointF(3.8 * u, -2.6 * u)}));
    }

    // "Co&zy: names and ..." -> the title "Cozy" (DemiBold, with the access key) and the rest under it.
    const QString all   = text();
    const int     colon = all.indexOf(QLatin1Char(':'));
    const QString title = colon > 0 ? all.left(colon) : all;
    QString       rest  = colon > 0 ? all.mid(colon + 1).trimmed() : QString();
    if (!rest.isEmpty())
        rest[0] = rest.at(0).toUpper();
    QFont bold = windowFont(this);
    bold.setWeight(QFont::DemiBold);
    p.setFont(bold);
    p.setPen(t.text);
    const QRectF        textBox(box.left() + t.px(10), art.bottom() + t.px(8), box.width() - t.px(20), box.bottom() - art.bottom() - t.px(10));
    const QFontMetricsF bfm(bold);
    p.drawText(textBox, Qt::AlignLeft | Qt::AlignTop | (showMnemonics(this) ? Qt::TextShowMnemonic : Qt::TextHideMnemonic), title);
    p.setFont(windowFont(this));
    p.setPen(t.muted);
    p.drawText(textBox.adjusted(0, bfm.height() + 1, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, rest);
    if (settingstheme::showsFocus(this))
        focusRing(p, box.adjusted(-0.5, -0.5, 0.5, 0.5), t.px(9), t);
}

// ---- TileLayout -------------------------------------------------------------------------------------------------------
TileLayout::TileLayout(QWidget* parent)
    : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
}

TileLayout::~TileLayout()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void TileLayout::addItem(QLayoutItem* item)
{
    m_items.append(item);
}

QLayoutItem* TileLayout::takeAt(int index)
{
    if (index < 0 || index >= m_items.size())
        return nullptr;
    return m_items.takeAt(index);
}

int TileLayout::gap() const
{
    return settingstheme::tokensFor(parentWidget()).px(10);
}

int TileLayout::tileHeight() const
{
    int h = 0;
    for (QLayoutItem* item : m_items)
        h = qMax(h, item->sizeHint().height());
    return h;
}

int TileLayout::perRow(int width) const
{
    int minimum = 0;
    for (QLayoutItem* item : m_items)
        minimum = qMax(minimum, item->minimumSize().width());
    const int n = m_items.size();
    return n > 1 && n * minimum + (n - 1) * gap() <= width ? n : 1;
}

QSize TileLayout::sizeHint() const
{
    int w = 0;
    for (QLayoutItem* item : m_items)
        w += item->sizeHint().width();
    w += qMax(0, m_items.size() - 1) * gap();
    return QSize(w, tileHeight());
}

QSize TileLayout::minimumSize() const
{
    int w = 0;
    for (QLayoutItem* item : m_items)
        w = qMax(w, item->minimumSize().width());
    return QSize(w, tileHeight());
}

int TileLayout::heightForWidth(int width) const
{
    const int per  = perRow(width);
    const int rows = (m_items.size() + per - 1) / qMax(1, per);
    return rows * tileHeight() + qMax(0, rows - 1) * gap();
}

void TileLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    const int per = perRow(rect.width());
    const int g   = gap();
    const int h   = tileHeight();
    const int w   = per > 1 ? (rect.width() - (per - 1) * g) / per : rect.width();
    for (int i = 0; i < m_items.size(); ++i) {
        const int col = i % per, row = i / per;
        m_items.at(i)->setGeometry(QRect(rect.left() + col * (w + g), rect.top() + row * (h + g), w, h));
    }
}

// ---- IconButton -------------------------------------------------------------------------------------------------------
IconButton::IconButton(settingsicons::Icon icon, QWidget* parent)
    : QPushButton(parent)
    , m_icon(icon)
{
    setAutoDefault(false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
}

QSize IconButton::sizeHint() const
{
    const int s = QFontMetrics(windowFont(this)).height() + settingstheme::tokensFor(this).px(14);
    return QSize(s, s);
}

void IconButton::enterEvent(QEvent* event)
{
    update();
    QPushButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event)
{
    update();
    QPushButton::leaveEvent(event);
}

void IconButton::paintEvent(QPaintEvent*)
{
    const Tokens t = settingstheme::tokensFor(this);
    QPainter     p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r  = QRectF(rect()).adjusted(t.px(1), t.px(1), -t.px(1), -t.px(1));
    const bool   en = isEnabled();
    p.setPen(QPen(en ? t.control : t.disabled, 1));
    p.setBrush(!en ? t.card : isDown() ? mix(t.card, t.text, 0.12) : underMouse() ? mix(t.card, t.text, 0.07) : t.card);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), t.px(6), t.px(6));
    const qreal s = r.height() * 0.62;
    settingsicons::draw(p, m_icon, QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s), en ? t.text : t.disabled);
    if (settingstheme::showsFocus(this))
        focusRing(p, r.adjusted(-t.px(2), -t.px(2), t.px(2), t.px(2)), t.px(8), t);
}

// ---- LinePreview ------------------------------------------------------------------------------------------------------
LinePreview::LinePreview(QWidget* parent)
    : QWidget(parent)
    , m_base(Qt::white)
    , m_text(Qt::black)
    , m_accent(0x47, 0x52, 0xc4)
    , m_warning(0xb5, 0x47, 0x08)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void LinePreview::setColors(const QColor& base, const QColor& text, const QColor& accent, const QColor& warning)
{
    m_base    = base;
    m_text    = text;
    m_accent  = accent;
    m_warning = warning;
    update();
}

QSize LinePreview::sizeHint() const
{
    const Tokens t = settingstheme::tokensFor(this);
    return QSize(t.px(320), QFontMetrics(windowFont(this)).height() * 2 + t.px(22));
}

QSize LinePreview::minimumSizeHint() const
{
    return QSize(settingstheme::tokensFor(this).px(120), sizeHint().height());
}

void LinePreview::paintEvent(QPaintEvent*)
{
    const Tokens t = settingstheme::tokensFor(this);
    QPainter     p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(t.border, 1));
    p.setBrush(m_base);
    p.drawRoundedRect(r, t.px(6), t.px(6));
    const QFont f = windowFont(this);
    QFont       b = f;
    b.setBold(true);
    const QFontMetrics fm(f), bm(b);
    const qreal        right = r.right() - t.px(10);
    // As TeamSpeak prints it: the time, the bold prefix in the line colour, the text in the chat's.
    const auto line = [&](qreal y, const QString& prefix, const QColor& prefixColor, const QString& body) {
        qreal x = r.left() + t.px(10);
        p.setFont(f);
        p.setPen(mix(m_text, m_base, 0.4));
        const QString time = QString::fromLatin1("<21:04>");
        p.drawText(QPointF(x, y), time);
        x += fm.horizontalAdvance(time) + t.px(6);
        p.setFont(b);
        p.setPen(prefixColor);
        p.drawText(QPointF(x, y), bm.elidedText(prefix, Qt::ElideRight, qMax(0, qRound(right - x))));
        x += bm.horizontalAdvance(prefix) + t.px(6);
        p.setFont(f);
        p.setPen(m_text);
        if (x < right)
            p.drawText(QPointF(x, y), fm.elidedText(body, Qt::ElideRight, qRound(right - x)));
    };
    const qreal y1 = r.top() + t.px(8) + fm.ascent();
    line(y1, i18n::t("TS Media chat"), m_accent, i18n::t("Sent holiday.jpg (2.4 MB)."));
    line(y1 + fm.height() + t.px(3), i18n::t("TS Media chat ⚠"), m_warning, i18n::t("The upload limit is 100 MB."));
}

// ---- SettingRow -------------------------------------------------------------------------------------------------------
SettingRow::SettingRow(Kind kind, int indent, QWidget* parent)
    : QWidget(parent)
    , m_kind(kind)
    , m_indent(qMax(0, indent))
{
    m_grid = new QGridLayout(this);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void SettingRow::setParts(QWidget* title, QLabel* hint, QWidget* control, QWidget* extra)
{
    m_title   = title;
    m_hint    = hint;
    m_control = control;
    m_extra   = extra;
    for (QWidget* w : {m_title, static_cast<QWidget*>(m_hint), m_control, m_extra}) {
        if (w && w->parentWidget() != this)
            w->setParent(this);
    }
    if (m_kind == Kind::Switch && m_control)
        m_control->raise(); // above the switch, which spans the row (its title isn't under the control)
    applyMetrics();
    arrange();
}

void SettingRow::setAlwaysStacked(bool on)
{
    m_always = on;
    arrange();
}

bool SettingRow::wantsStacked(int width) const
{
    if (m_kind != Kind::Field || !m_control)
        return false;
    return m_always || m_control->sizeHint().expandedTo(m_control->minimumSize()).width() > 0.4 * width;
}

bool SettingRow::isFirstInCard() const
{
    const QWidget* card = parentWidget();
    if (!card || !card->layout())
        return true;
    for (int i = 0; i < card->layout()->count(); ++i) {
        QWidget* w = card->layout()->itemAt(i)->widget();
        if (w && !w->isHidden())
            return w == this;
    }
    return true;
}

void SettingRow::applyMetrics()
{
    const Tokens t = settingstheme::tokensFor(this);
    m_grid->setContentsMargins(t.px(14) + m_indent * t.px(28), t.px(9), t.px(10), t.px(9));
    m_grid->setVerticalSpacing(t.px(2));
    m_grid->setHorizontalSpacing(m_kind == Kind::Switch ? t.px(12) : t.px(16));
    if (m_kind == Kind::Switch) {
        // The switch's column holds the track, the room for its focus ring and the gap to a trailing control (an empty
        // column gets no spacing of its own).
        const QSize track = SwitchBox::trackSize(windowFont(this), t.k);
        m_grid->setColumnMinimumWidth(2, track.width() + t.px(4) + t.px(12));
        if (m_hint) // the helper never runs under the switch
            m_hint->setContentsMargins(0, 0, track.width() + t.px(16) + t.px(4), 0);
        if (auto* box = qobject_cast<SwitchBox*>(m_title))
            box->setTrailingReserve(m_control ? m_control->sizeHint().width() + t.px(12) : 0);
    }
    if (m_kind == Kind::Stacked || (m_kind == Kind::Field && m_stacked))
        m_grid->setVerticalSpacing(t.px(4));
}

void SettingRow::arrange()
{
    if (m_kind == Kind::Field)
        m_stacked = m_always || (m_arranged ? wantsStacked(width()) : m_stacked);
    for (QWidget* w : {m_title, static_cast<QWidget*>(m_hint), m_control, m_extra}) {
        if (w)
            m_grid->removeWidget(w);
    }
    for (int c = 0; c < 3; ++c) {
        m_grid->setColumnStretch(c, 0);
    }
    switch (m_kind) {
    case Kind::Switch:
        // The trailing control first: the Tab order follows the layout (trailing field, then its switch).
        if (m_control)
            m_grid->addWidget(m_control, 0, 1, Qt::AlignVCenter);
        if (m_title)
            m_grid->addWidget(m_title, 0, 0, 1, 3);
        if (m_hint)
            m_grid->addWidget(m_hint, 1, 0, 1, 3);
        m_grid->setColumnStretch(0, 1);
        break;
    case Kind::Field:
        if (m_title)
            m_grid->addWidget(m_title, 0, 0, m_hint ? Qt::AlignBottom : Qt::AlignVCenter);
        if (m_hint)
            m_grid->addWidget(m_hint, 1, 0);
        if (m_control) {
            if (m_stacked)
                m_grid->addWidget(m_control, 2, 0, Qt::AlignLeft | Qt::AlignVCenter);
            else
                m_grid->addWidget(m_control, 0, 1, m_hint ? 2 : 1, 1, Qt::AlignRight | Qt::AlignVCenter);
        }
        m_grid->setColumnStretch(0, 1);
        break;
    case Kind::Stacked:
        if (m_title)
            m_grid->addWidget(m_title, 0, 0);
        if (m_control)
            m_grid->addWidget(m_control, 1, 0);
        if (m_extra)
            m_grid->addWidget(m_extra, 2, 0);
        if (m_hint)
            m_grid->addWidget(m_hint, 3, 0);
        m_grid->setColumnStretch(0, 1);
        break;
    case Kind::Custom:
        if (m_control)
            m_grid->addWidget(m_control, 0, 0);
        m_grid->setColumnStretch(0, 1);
        break;
    }
    applyMetrics();
    m_arranged = true;
}

bool SettingRow::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Polish:
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::ParentChange:
        applyMetrics();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void SettingRow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_kind == Kind::Field && !m_always && wantsStacked(width()) != m_stacked)
        arrange(); // M9: a wide control moves under its title and hint
}

void SettingRow::paintEvent(QPaintEvent*)
{
    if (isFirstInCard())
        return;
    const Tokens t = settingstheme::tokensFor(this);
    QPainter     p(this);
    const int    left = t.px(14) + m_indent * t.px(28);
    p.fillRect(QRect(left, 0, width() - left, 1), t.divider); // 1 px at every scale
}

// ---- NavDelegate ------------------------------------------------------------------------------------------------------
QSize NavDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const
{
    const Tokens t = settingstheme::tokensFor(option.widget);
    const QFont  f = windowFont(option.widget);
    return QSize(t.px(160), qMax(t.px(34), QFontMetrics(f).height() + t.px(18)));
}

void NavDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const Tokens t = settingstheme::tokensFor(option.widget);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r     = QRectF(option.rect).adjusted(t.px(8), t.px(2), -t.px(8), -t.px(2));
    const bool   sel   = option.state & QStyle::State_Selected;
    const bool   hover = option.state & QStyle::State_MouseOver;
    if (sel || hover) {
        p->setPen(Qt::NoPen);
        p->setBrush(sel ? t.navSel : t.navHover);
        p->drawRoundedRect(r, t.px(6), t.px(6));
    }
    if (sel) { // the accent bar: an extra cue, the item is tinted and semibold too
        p->setPen(Qt::NoPen);
        p->setBrush(t.accent);
        const qreal h = r.height() * 0.44;
        p->drawRoundedRect(QRectF(r.left() + 1, r.center().y() - h / 2, t.px(3), h), t.px(3) / 2.0, t.px(3) / 2.0);
    }
    const qreal is = t.px(18);
    settingsicons::draw(*p, static_cast<settingsicons::Icon>(index.data(Qt::UserRole).toInt()), QRectF(r.left() + t.px(12), r.center().y() - is / 2, is, is),
                        t.text);
    QFont f = windowFont(option.widget);
    if (sel)
        f.setWeight(QFont::DemiBold);
    p->setFont(f);
    p->setPen(t.text);
    const QRectF textRect = r.adjusted(t.px(12) + is + t.px(10), 0, -t.px(6), 0);
    p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(index.data().toString(), Qt::ElideRight, qRound(textRect.width())));
    if ((option.state & QStyle::State_HasFocus) && settingstheme::showsFocus(option.widget))
        focusRing(*p, r.adjusted(-1, -1, 1, 1), t.px(7), t);
    p->restore();
}
