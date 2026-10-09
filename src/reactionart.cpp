#include "reactionart.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtMath>

#include <cmath>

#include "i18n.h"
#include "uiutil.h"

namespace rx {

namespace {

// The pictures are designed on a 36 x 36 grid and scaled to their box.
constexpr qreal kGrid = 36.0;

const QColor kAccent(0x58, 0x65, 0xf2);
const QColor kSkin(0xff, 0xcc, 0x4d);     // faces and the hand
const QColor kSkinEdge(0xe0, 0x9a, 0x1f); // their rim and creases
const QColor kFeature(0x66, 0x45, 0x00);  // eyes, mouths, brows
const QColor kTear(0x5d, 0xad, 0xec);
const QColor kTearEdge(0x3b, 0x88, 0xc3);
const QColor kTongue(0xe8, 0x59, 0x6e);
const QColor kHeart(0xdd, 0x2e, 0x44);
const QColor kHeartEdge(0xb8, 0x1f, 0x34);
const QColor kFlame(0xf4, 0x90, 0x0c);
const QColor kFlameEdge(0xd8, 0x6a, 0x08);
const QColor kFlameCore(0xff, 0xcc, 0x4d);
const QColor kCuff(0x58, 0x65, 0xf2);

constexpr qreal kPillRadius   = 8.0;
constexpr qreal kPillPadLeft  = 6.0;
constexpr qreal kPillIcon     = 16.0;
constexpr qreal kPillIconGap  = 4.0;
constexpr qreal kPillPadRight = 8.0;
constexpr qreal kPillMinWidth = 40.0;

// The painter draws in grid units inside box until the scope ends.
class GridScope
{
  public:
    GridScope(QPainter& p, const QRectF& box)
        : m_p(p)
    {
        m_p.save();
        m_p.setRenderHint(QPainter::Antialiasing);
        m_p.translate(box.topLeft());
        m_p.scale(box.width() / kGrid, box.height() / kGrid);
    }
    ~GridScope() { m_p.restore(); }
    GridScope(const GridScope&)            = delete;
    GridScope& operator=(const GridScope&) = delete;

  private:
    QPainter& m_p;
};

QPen roundPen(const QColor& color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// A drop with its round body at center, pointing up before the rotation (degrees, clockwise).
QPainterPath tearDrop(const QPointF& center, qreal r, qreal rotation)
{
    QPainterPath path;
    const qreal  cx = center.x();
    const qreal  cy = center.y();
    path.moveTo(cx, cy - 2.1 * r);
    path.cubicTo(cx + 0.25 * r, cy - 1.45 * r, cx + r, cy - 0.85 * r, cx + r, cy);
    path.arcTo(QRectF(cx - r, cy - r, 2 * r, 2 * r), 0, -180);
    path.cubicTo(cx - r, cy - 0.85 * r, cx - 0.25 * r, cy - 1.45 * r, cx, cy - 2.1 * r);
    path.closeSubpath();
    QTransform t;
    t.translate(cx, cy);
    t.rotate(rotation);
    t.translate(-cx, -cy);
    return t.map(path);
}

void drawFace(QPainter& p)
{
    p.setPen(roundPen(kSkinEdge, 1.1));
    p.setBrush(kSkin);
    p.drawEllipse(QPointF(18, 18), 16.6, 16.6);
}

void drawTear(QPainter& p, const QPointF& center, qreal r, qreal rotation)
{
    p.setPen(roundPen(kTearEdge, 0.9));
    p.setBrush(kTear);
    p.drawPath(tearDrop(center, r, rotation));
}

void drawThumbsUp(QPainter& p)
{
    // Cuff on the left, the fist with four fingers, the thumb pointing up.
    p.setPen(Qt::NoPen);
    p.setBrush(kCuff);
    p.drawRoundedRect(QRectF(2.0, 17.0, 7.5, 17.0), 1.8, 1.8);

    // One outline for palm, thumb and fingers (united piece by piece: overlapping sub-paths of a
    // single path would cancel out under the odd-even rule).
    const auto rounded = [](const QRectF& r, qreal radius) {
        QPainterPath path;
        path.addRoundedRect(r, radius, radius);
        return path;
    };
    QPainterPath thumb;
    thumb.moveTo(9.0, 18.5);
    thumb.cubicTo(10.0, 13.5, 11.2, 9.0, 12.4, 5.6);
    thumb.cubicTo(13.4, 2.6, 18.2, 2.2, 19.4, 4.8);
    thumb.cubicTo(20.4, 7.0, 19.8, 10.4, 19.0, 13.6);
    thumb.lineTo(18.6, 16.0);
    thumb.lineTo(9.0, 18.5);
    thumb.closeSubpath();
    QPainterPath hand = rounded(QRectF(8.5, 15.0, 15.0, 19.0), 3.2);
    hand              = hand.united(thumb);
    // Four fingers, each a capsule, the longest second from the top.
    const qreal rows[4][2] = {{15.0, 31.0}, {19.75, 32.6}, {24.5, 32.0}, {29.25, 29.8}};
    for (const auto& row : rows)
        hand = hand.united(rounded(QRectF(16.0, row[0], row[1] - 16.0, 4.75), 2.375));
    hand = hand.simplified();

    p.setPen(roundPen(kSkinEdge, 1.1));
    p.setBrush(kSkin);
    p.drawPath(hand);

    // Where the fingers meet: short creases from the knuckles outwards, and one under the thumb.
    p.setBrush(Qt::NoBrush);
    p.setPen(roundPen(kSkinEdge, 1.0));
    for (int i = 1; i < 4; ++i) {
        const qreal y = rows[i][0];
        p.drawLine(QPointF(23.0, y), QPointF(qMin(rows[i - 1][1], rows[i][1]) - 1.8, y));
    }
    QPainterPath crease;
    crease.moveTo(18.8, 15.4);
    crease.quadTo(17.4, 17.2, 17.8, 19.4);
    p.drawPath(crease);
}

void drawHeart(QPainter& p)
{
    QPainterPath heart;
    heart.moveTo(18.0, 32.0);
    heart.cubicTo(13.5, 28.2, 2.5, 21.5, 2.5, 12.5);
    heart.cubicTo(2.5, 7.2, 6.6, 3.5, 11.0, 3.5);
    heart.cubicTo(14.4, 3.5, 16.8, 5.6, 18.0, 8.4);
    heart.cubicTo(19.2, 5.6, 21.6, 3.5, 25.0, 3.5);
    heart.cubicTo(29.4, 3.5, 33.5, 7.2, 33.5, 12.5);
    heart.cubicTo(33.5, 21.5, 22.5, 28.2, 18.0, 32.0);
    heart.closeSubpath();
    p.setPen(roundPen(kHeartEdge, 1.1));
    p.setBrush(kHeart);
    p.drawPath(heart);
    // A soft highlight.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 90));
    p.save();
    p.translate(9.5, 10.0);
    p.rotate(-35);
    p.drawEllipse(QPointF(0, 0), 3.4, 2.0);
    p.restore();
}

void drawLaughing(QPainter& p)
{
    drawFace(p);
    // Eyes squeezed shut.
    p.setBrush(Qt::NoBrush);
    p.setPen(roundPen(kFeature, 2.4));
    QPainterPath eyes;
    eyes.moveTo(8.6, 15.2);
    eyes.quadTo(12.2, 9.4, 15.6, 15.2);
    eyes.moveTo(20.4, 15.2);
    eyes.quadTo(23.8, 9.4, 27.4, 15.2);
    p.drawPath(eyes);

    // Wide open laugh with teeth and tongue.
    QPainterPath mouth;
    mouth.moveTo(7.6, 19.6);
    mouth.quadTo(18.0, 21.4, 28.4, 19.6);
    mouth.cubicTo(28.0, 27.6, 23.4, 32.2, 18.0, 32.2);
    mouth.cubicTo(12.6, 32.2, 8.0, 27.6, 7.6, 19.6);
    mouth.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawPath(mouth);
    p.save();
    p.setClipPath(mouth);
    p.setBrush(Qt::white);
    p.drawRect(QRectF(6, 18, 24, 5.4));
    p.setBrush(kTongue);
    p.drawEllipse(QPointF(18.0, 31.0), 6.2, 3.6);
    p.restore();

    // Tears of joy.
    drawTear(p, QPointF(5.2, 21.5), 3.3, 28);
    drawTear(p, QPointF(30.8, 21.5), 3.3, -28);
}

void drawSurprised(QPainter& p)
{
    drawFace(p);
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawEllipse(QPointF(12.3, 14.2), 2.4, 3.4);
    p.drawEllipse(QPointF(23.7, 14.2), 2.4, 3.4);
    // Raised brows.
    p.setBrush(Qt::NoBrush);
    p.setPen(roundPen(kFeature, 1.8));
    QPainterPath brows;
    brows.moveTo(8.6, 8.6);
    brows.quadTo(11.6, 5.8, 15.0, 7.4);
    brows.moveTo(27.4, 8.6);
    brows.quadTo(24.4, 5.8, 21.0, 7.4);
    p.drawPath(brows);
    // "O" mouth.
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawEllipse(QPointF(18.0, 25.8), 4.4, 5.2);
}

void drawSad(QPainter& p)
{
    drawFace(p);
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawEllipse(QPointF(12.3, 16.0), 2.3, 3.1);
    p.drawEllipse(QPointF(23.7, 16.0), 2.3, 3.1);
    p.setBrush(Qt::NoBrush);
    p.setPen(roundPen(kFeature, 1.9));
    QPainterPath face;
    // Worried brows (the inner ends up) and a frown.
    face.moveTo(7.8, 11.4);
    face.lineTo(14.2, 9.0);
    face.moveTo(28.2, 11.4);
    face.lineTo(21.8, 9.0);
    face.moveTo(12.2, 27.6);
    face.quadTo(18.0, 22.2, 23.8, 27.6);
    p.setPen(roundPen(kFeature, 2.2));
    p.drawPath(face);
    drawTear(p, QPointF(25.6, 24.4), 2.9, 0);
}

void drawFire(QPainter& p)
{
    QPainterPath outer;
    outer.moveTo(18.0, 34.0);
    outer.cubicTo(10.8, 34.0, 5.6, 29.4, 5.6, 22.8);
    outer.cubicTo(5.6, 17.0, 9.4, 13.4, 11.6, 9.0);
    outer.cubicTo(12.4, 12.0, 13.8, 13.8, 15.6, 14.6);
    outer.cubicTo(15.0, 9.2, 17.0, 4.6, 20.6, 1.8);
    outer.cubicTo(21.0, 6.8, 24.0, 10.2, 26.8, 13.6);
    outer.cubicTo(29.4, 16.8, 30.4, 19.6, 30.4, 22.8);
    outer.cubicTo(30.4, 29.4, 25.2, 34.0, 18.0, 34.0);
    outer.closeSubpath();
    p.setPen(roundPen(kFlameEdge, 1.1));
    p.setBrush(kFlame);
    p.drawPath(outer);

    QPainterPath core;
    core.moveTo(18.0, 33.4);
    core.cubicTo(14.0, 33.4, 11.4, 30.6, 11.4, 27.2);
    core.cubicTo(11.4, 23.8, 13.8, 21.8, 15.4, 18.8);
    core.cubicTo(16.0, 21.2, 17.4, 22.6, 18.8, 23.0);
    core.cubicTo(18.8, 20.4, 20.4, 18.2, 22.2, 16.8);
    core.cubicTo(23.4, 19.8, 24.6, 23.0, 24.6, 27.2);
    core.cubicTo(24.6, 30.6, 22.0, 33.4, 18.0, 33.4);
    core.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(kFlameCore);
    p.drawPath(core);
}

qreal basePixelSize(const QFont& font)
{
    qreal px = 12.0;
    if (font.pixelSize() > 0)
        px = font.pixelSize();
    else if (font.pointSizeF() > 0)
        px = font.pointSizeF() * 4.0 / 3.0;
    return qBound(11.0, px, 15.0);
}

qreal pillWidth(const ReactionView::Entry& entry, const MeasureText& measure)
{
    const qreal text = std::ceil(measure ? measure(countText(entry.count), entry.mine) : 7.0 * countText(entry.count).size());
    return qMax(kPillMinWidth, kPillPadLeft + kPillIcon + kPillIconGap + text + kPillPadRight);
}

QColor shade(const QColor& color, qreal amount, bool lighter)
{
    // Mixes towards white or black by amount (0..1).
    const QColor target = lighter ? QColor(255, 255, 255) : QColor(0, 0, 0);
    return QColor(qRound(color.red() + (target.red() - color.red()) * amount), qRound(color.green() + (target.green() - color.green()) * amount),
                  qRound(color.blue() + (target.blue() - color.blue()) * amount));
}

} // namespace

QString reactionName(int index)
{
    switch (index) {
    case proto::ThumbsUp:
        return i18n::t("Thumbs up");
    case proto::Heart:
        return i18n::t("Heart");
    case proto::Laughing:
        return i18n::t("Laughing");
    case proto::Surprised:
        return i18n::t("Surprised");
    case proto::Sad:
        return i18n::t("Sad");
    case proto::Fire:
        return i18n::t("Fire");
    default:
        return {};
    }
}

void drawReaction(QPainter& p, const QRectF& box, int index)
{
    GridScope grid(p, box);
    switch (index) {
    case proto::ThumbsUp:
        drawThumbsUp(p);
        break;
    case proto::Heart:
        drawHeart(p);
        break;
    case proto::Laughing:
        drawLaughing(p);
        break;
    case proto::Surprised:
        drawSurprised(p);
        break;
    case proto::Sad:
        drawSad(p);
        break;
    case proto::Fire:
        drawFire(p);
        break;
    default:
        break;
    }
}

void drawAddReactionGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    // Drawn on a 16 grid: a smiley whose outline opens towards the plus at the top right.
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 16.0, box.height() / 16.0);
    p.setPen(roundPen(color, 1.5)); // grid units: 1.5 px at 16 px, like the other line icons
    p.setBrush(Qt::NoBrush);
    const QRectF face(1.0, 3.0, 12.0, 12.0);
    QPainterPath outline;
    outline.arcMoveTo(face, 80);
    outline.arcTo(face, 80, 300);
    p.drawPath(outline);
    QPainterPath smile;
    smile.moveTo(4.4, 10.2);
    smile.quadTo(7.0, 12.8, 9.6, 10.2);
    p.drawPath(smile);
    p.drawLine(QPointF(13.0, 0.9), QPointF(13.0, 5.9));
    p.drawLine(QPointF(10.5, 3.4), QPointF(15.5, 3.4));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(5.0, 7.6), 1.0, 1.0);
    p.drawEllipse(QPointF(9.0, 7.6), 1.0, 1.0);
    p.restore();
}

void drawPeopleGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 16.0, box.height() / 16.0);
    p.setPen(roundPen(color, 1.5));
    p.setBrush(Qt::NoBrush);
    // Front person.
    p.drawEllipse(QPointF(6.0, 5.2), 2.6, 2.6);
    QPainterPath body;
    body.moveTo(1.2, 14.4);
    body.cubicTo(1.2, 11.0, 3.4, 9.6, 6.0, 9.6);
    body.cubicTo(8.6, 9.6, 10.8, 11.0, 10.8, 14.4);
    p.drawPath(body);
    // The one behind.
    QPainterPath back;
    back.moveTo(10.6, 2.9);
    back.cubicTo(12.6, 2.6, 14.0, 4.0, 13.6, 5.9);
    back.cubicTo(13.4, 7.0, 12.5, 7.8, 11.4, 7.9);
    back.moveTo(12.9, 9.8);
    back.cubicTo(14.4, 10.4, 15.0, 11.8, 15.0, 14.4);
    p.drawPath(back);
    p.restore();
}

// ---- the row --------------------------------------------------------------------------------------

QString countText(int count)
{
    if (count > 99)
        return QString::fromLatin1("99+"); // may end up in a menu text: no DLL-backed data
    return QString::number(qMax(0, count));
}

QFont countFont(const QFont& chatFont, bool bold)
{
    QFont f = chatFont;
    f.setPixelSize(qMax(8, qRound(basePixelSize(chatFont))));
    f.setWeight(bold ? QFont::Bold : QFont::Medium);
    f.setItalic(false);
    f.setUnderline(false);
    f.setStrikeOut(false);
    return f;
}

RowLayout layoutRow(const ReactionView& view, int pictureWidth, int maxWidth, const MeasureText& measure, bool keep)
{
    RowLayout row;
    QVector<QPair<int, qreal>> pills; // reaction, width
    for (int i = 0; i < proto::kReactionCount; ++i) {
        if (view.per[i].count > 0)
            pills.append({i, pillWidth(view.per[i], measure)});
    }
    if (pills.isEmpty() && !keep)
        return row;

    qreal oneLine = kAddPillWidth;
    for (const auto& pill : qAsConst(pills))
        oneLine += pill.second + kPillGap;
    const qreal limit = qMax<qreal>(kAddPillWidth, maxWidth);
    const qreal width = qMax<qreal>(pictureWidth, qMin(oneLine, limit));

    qreal      x     = 0;
    qreal      y     = kRowTopMargin;
    const auto place = [&](qreal w) {
        if (x > 0 && x + w > width + 0.01) {
            x = 0;
            y += kPillHeight + kLineGap;
        }
        const QRectF r(x, y, w, kPillHeight);
        x += w + kPillGap;
        return r;
    };
    for (const auto& pill : qAsConst(pills)) {
        row.pills.append(place(pill.second));
        row.reactions.append(pill.first);
    }
    row.addPill = place(kAddPillWidth);
    row.size    = QSize(qCeil(width), qCeil(y + kPillHeight));
    return row;
}

QRectF addButtonRect(const QSizeF& picture)
{
    return QRectF(picture.width() - kAddButtonInset - kAddButton, kAddButtonInset, kAddButton, kAddButton);
}

bool addButtonFits(const QSizeF& picture)
{
    return picture.width() >= kAddButton + 2 * kAddButtonInset + 16 && picture.height() >= kAddButton + 2 * kAddButtonInset;
}

ZoneHit zoneAt(const QSizeF& picture, const RowLayout& row, bool addButton, const QPointF& pos)
{
    ZoneHit hit;
    const QRectF pictureRect(QPointF(0, 0), picture);
    if (pictureRect.contains(pos)) {
        const QRectF button = addButtonRect(picture);
        // A little larger than drawn: the round button's corners count too.
        if (addButton && addButtonFits(picture) && button.adjusted(-2, -2, 2, 2).contains(pos)) {
            hit.zone = Zone::AddButton;
            hit.rect = button;
            return hit;
        }
        hit.zone = Zone::Picture;
        hit.rect = pictureRect;
        return hit;
    }
    if (row.isEmpty())
        return hit;
    const QPointF local = pos - QPointF(0, picture.height());
    if (!QRectF(QPointF(0, 0), QSizeF(row.size)).contains(local))
        return hit;
    for (int i = 0; i < row.pills.size(); ++i) {
        if (row.pills.at(i).contains(local)) {
            hit.zone  = Zone::Pill;
            hit.index = row.reactions.at(i);
            hit.rect  = row.pills.at(i).translated(0, picture.height());
            return hit;
        }
    }
    if (row.addPill.contains(local)) {
        hit.zone = Zone::AddPill;
        hit.rect = row.addPill.translated(0, picture.height());
        return hit;
    }
    hit.zone = Zone::Row;
    hit.rect = QRectF(QPointF(0, picture.height()), QSizeF(row.size));
    return hit;
}

RowColors rowColors(bool dark, const QColor& base)
{
    RowColors c;
    if (dark) {
        const QColor bg = base.isValid() ? base : QColor(0x31, 0x33, 0x38);
        c.pill          = ui::flatten(QColor(255, 255, 255, 15), bg);
        c.pillHover     = ui::flatten(QColor(255, 255, 255, 30), bg);
        c.pillPressed   = ui::flatten(QColor(255, 255, 255, 41), bg);
        c.border        = ui::flatten(QColor(255, 255, 255, 26), bg);
        c.count         = QColor(0xdb, 0xde, 0xe1);
        c.mine          = ui::flatten(QColor(88, 101, 242, 77), bg);
        c.mineHover     = ui::flatten(QColor(88, 101, 242, 102), bg);
        c.minePressed   = ui::flatten(QColor(88, 101, 242, 122), bg);
        c.mineBorder    = QColor(0x79, 0x84, 0xf5); // the accent, lighter: 3:1 on dark chats
        c.mineCount     = QColor(0xff, 0xff, 0xff);
        c.addIcon       = QColor(0xb5, 0xba, 0xc1);
    } else {
        c.pill        = QColor(0xf2, 0xf3, 0xf5);
        c.pillHover   = shade(c.pill, 0.06, false);
        c.pillPressed = shade(c.pill, 0.10, false);
        c.border      = QColor(0xe3, 0xe5, 0xe8);
        c.count       = QColor(0x31, 0x33, 0x38);
        c.mine        = QColor(0xe6, 0xe8, 0xfd);
        c.mineHover   = shade(c.mine, 0.05, false);
        c.minePressed = shade(c.mine, 0.09, false);
        c.mineBorder  = kAccent;
        c.mineCount   = QColor(0x3b, 0x45, 0xc4);
        c.addIcon     = QColor(0x4e, 0x50, 0x58);
    }
    return c;
}

void drawRow(QPainter& p, const QPointF& origin, const RowLayout& row, const ReactionView& view, const RowPaint& paint, const RowColors& colors,
             const QFont& chatFont)
{
    if (row.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.translate(origin);
    const QFont normal = countFont(chatFont, false);
    const QFont bold   = countFont(chatFont, true);

    for (int i = 0; i < row.pills.size(); ++i) {
        const int                  reaction = row.reactions.at(i);
        const ReactionView::Entry& entry    = view.per[reaction];
        const bool                 mine     = entry.mine;
        const bool                 pressed  = paint.pressedReaction == reaction;
        const bool                 hovered  = paint.hoverReaction == reaction;
        QColor                     fill     = mine ? colors.mine : colors.pill;
        if (pressed)
            fill = mine ? colors.minePressed : colors.pillPressed;
        else if (hovered)
            fill = mine ? colors.mineHover : colors.pillHover;
        const qreal  borderWidth = mine ? 1.5 : 1.0;
        const QRectF pill        = row.pills.at(i).adjusted(borderWidth / 2, borderWidth / 2, -borderWidth / 2, -borderWidth / 2);
        p.setPen(QPen(mine ? colors.mineBorder : colors.border, borderWidth));
        p.setBrush(fill);
        p.drawRoundedRect(pill, kPillRadius, kPillRadius);

        const QRectF r = row.pills.at(i);
        drawReaction(p, QRectF(r.left() + kPillPadLeft, r.top() + (r.height() - kPillIcon) / 2, kPillIcon, kPillIcon), reaction);
        p.setFont(mine ? bold : normal);
        p.setPen(mine ? colors.mineCount : colors.count);
        const QRectF text(r.left() + kPillPadLeft + kPillIcon + kPillIconGap, r.top(), r.width() - kPillPadLeft - kPillIcon - kPillIconGap - kPillPadRight + 2,
                          r.height());
        p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, countText(entry.count));
    }

    if (paint.showAdd) {
        const QRectF add  = row.addPill.adjusted(0.5, 0.5, -0.5, -0.5);
        QColor       fill = colors.pill;
        if (paint.addPressed)
            fill = colors.pillPressed;
        else if (paint.addHover)
            fill = colors.pillHover;
        p.setPen(QPen(colors.border, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(add, kPillRadius, kPillRadius);
        const QRectF r = row.addPill;
        drawAddReactionGlyph(p, QRectF(r.center().x() - 8, r.center().y() - 8, 16, 16), colors.addIcon);
    }
    p.restore();
}

void drawAddButton(QPainter& p, const QSizeF& picture, bool hover, bool pressed)
{
    if (!addButtonFits(picture))
        return;
    const QRectF disc = addButtonRect(picture);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, pressed ? 217 : hover ? 184 : 153));
    p.drawEllipse(disc);
    drawAddReactionGlyph(p, QRectF(disc.center().x() - 8, disc.center().y() - 8, 16, 16), Qt::white);
    p.restore();
}

QImage composeObject(const QImage& picture, const QSize& pictureSize, const ReactionView& view, const ObjectState& state, ObjectLayout* layout,
                     QSize* objectSize)
{
    ObjectLayout out;
    out.picture = pictureSize;
    *objectSize = pictureSize;
    if (picture.isNull()) {
        if (layout)
            *layout = out;
        return picture;
    }
    const QFontMetricsF normal(countFont(state.font, false));
    const QFontMetricsF bold(countFont(state.font, true));
    const MeasureText   measure = [&](const QString& text, bool isBold) { return (isBold ? bold : normal).horizontalAdvance(text); };

    const bool add = state.hovered && state.canAdd;
    out.row        = layoutRow(view, pictureSize.width(), state.maxWidth, measure, state.keep);
    out.button     = add && !state.noButton && out.row.isEmpty() && addButtonFits(QSizeF(pictureSize));
    if (layout)
        *layout = out;
    if (out.row.isEmpty() && !out.button)
        return picture;

    const QSize object(qMax(pictureSize.width(), out.row.size.width()), pictureSize.height() + out.row.size.height());
    const qreal dpr = state.dpr > 0 ? state.dpr : 1.0;
    QImage      canvas(QSize(qMax(1, qRound(object.width() * dpr)), qMax(1, qRound(object.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(QPointF(0, 0), QSizeF(pictureSize)), picture);
    if (out.button) {
        const bool over = state.hoverZone == Zone::AddButton;
        drawAddButton(p, QSizeF(pictureSize), over, over && state.pressZone == Zone::AddButton);
    }
    if (!out.row.isEmpty()) {
        RowPaint paint;
        paint.showAdd = add;
        if (state.hoverZone == Zone::Pill)
            paint.hoverReaction = state.hoverIndex;
        if (state.pressZone == Zone::Pill && state.hoverZone == Zone::Pill && state.hoverIndex == state.pressIndex)
            paint.pressedReaction = state.pressIndex;
        paint.addHover   = state.hoverZone == Zone::AddPill;
        paint.addPressed = paint.addHover && state.pressZone == Zone::AddPill;
        drawRow(p, QPointF(0, pictureSize.height()), out.row, view, paint, rowColors(state.dark, state.base), state.font);
    }
    p.end();
    *objectSize = object;
    return canvas;
}

QVector<ColorPair> colorPairs(bool dark, const QColor& base)
{
    const RowColors    c = rowColors(dark, base);
    QVector<ColorPair> pairs;
    pairs.append({QStringLiteral("reaction count"), c.count, c.pill, 4.5});
    pairs.append({QStringLiteral("reaction count, hover"), c.count, c.pillHover, 4.5});
    pairs.append({QStringLiteral("reaction count, pressed"), c.count, c.pillPressed, 4.5});
    pairs.append({QStringLiteral("own reaction count"), c.mineCount, c.mine, 4.5});
    pairs.append({QStringLiteral("own reaction count, hover"), c.mineCount, c.mineHover, 4.5});
    pairs.append({QStringLiteral("own reaction count, pressed"), c.mineCount, c.minePressed, 4.5});
    pairs.append({QStringLiteral("own reaction border"), c.mineBorder, base.isValid() ? base : (dark ? QColor(0x31, 0x33, 0x38) : QColor(Qt::white)), 3.0});
    pairs.append({QStringLiteral("add reaction icon"), c.addIcon, c.pill, 3.0});
    pairs.append({QStringLiteral("add reaction icon, hover"), c.addIcon, c.pillHover, 3.0});
    pairs.append({QStringLiteral("add button over white"), QColor(Qt::white), ui::flatten(QColor(0, 0, 0, 153), QColor(Qt::white)), 3.0});
    pairs.append({QStringLiteral("add button over black"), QColor(Qt::white), ui::flatten(QColor(0, 0, 0, 153), QColor(Qt::black)), 3.0});
    return pairs;
}

} // namespace rx
