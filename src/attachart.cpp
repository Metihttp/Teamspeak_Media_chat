#include "attachart.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace attachart {

namespace {

constexpr qreal kDesign = 24.0; // every glyph is drawn in a 24-unit box and scaled

// A paperclip (the centre line of the wire, stroked), leaning right like the attach icons people know.
QPainterPath paperclipLine()
{
    QPainterPath c;
    c.moveTo(16.75, 7.0);
    c.lineTo(16.75, 16.5);
    c.arcTo(QRectF(7.25, 11.75, 9.5, 9.5), 0, -180); // the outer bend at the bottom
    c.lineTo(7.25, 5.75);
    c.arcTo(QRectF(7.25, 2.5, 6.5, 6.5), 180, -180); // the bend at the top
    c.lineTo(13.75, 16.0);
    c.arcTo(QRectF(10.25, 14.25, 3.5, 3.5), 0, -180); // the inner bend
    c.lineTo(10.25, 8.25);
    QTransform t;
    t.translate(12.0, 12.0);
    t.rotate(45.0);
    t.translate(-12.0, -11.875);
    return t.map(c);
}

void strokeIn(QPainter& p, const QPainterPath& path, const QColor& color, qreal width)
{
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

} // namespace

void draw(QPainter& p, const QRectF& box, Glyph glyph, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    const qreal scale = qMin(box.width(), box.height()) / kDesign;
    p.scale(scale, scale);
    switch (glyph) {
    case Glyph::Paperclip: {
        static const QPainterPath clip = paperclipLine();
        strokeIn(p, clip, color, 1.95); // as heavy as the microphone next to it
        break;
    }
    case Glyph::Photo: {
        QPainterPath frame;
        frame.addRoundedRect(QRectF(3.5, 5.0, 17.0, 14.0), 3.2, 3.2);
        strokeIn(p, frame, color, 1.8);
        QPainterPath hills;
        hills.moveTo(6.4, 16.4);
        hills.lineTo(10.3, 12.3);
        hills.lineTo(13.4, 15.4);
        hills.lineTo(15.3, 13.5);
        hills.lineTo(17.6, 15.8);
        strokeIn(p, hills, color, 1.8);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(15.6, 9.3), 1.75, 1.75);
        break;
    }
    case Glyph::Document: {
        QPainterPath page;
        page.moveTo(13.6, 3.2);
        page.lineTo(7.0, 3.2);
        page.quadTo(5.6, 3.2, 5.6, 4.6);
        page.lineTo(5.6, 19.4);
        page.quadTo(5.6, 20.8, 7.0, 20.8);
        page.lineTo(17.0, 20.8);
        page.quadTo(18.4, 20.8, 18.4, 19.4);
        page.lineTo(18.4, 8.0);
        page.closeSubpath(); // the fold's diagonal
        strokeIn(p, page, color, 1.8);
        QPainterPath fold;
        fold.moveTo(13.6, 3.6);
        fold.lineTo(13.6, 6.8);
        fold.quadTo(13.6, 8.0, 14.8, 8.0);
        fold.lineTo(18.0, 8.0);
        strokeIn(p, fold, color, 1.8);
        QPainterPath lines;
        lines.moveTo(9.0, 12.6);
        lines.lineTo(15.0, 12.6);
        lines.moveTo(9.0, 16.4);
        lines.lineTo(13.2, 16.4);
        strokeIn(p, lines, color, 1.8);
        break;
    }
    case Glyph::Poll: {
        QPainterPath bars;
        bars.moveTo(6.6, 18.6);
        bars.lineTo(6.6, 12.4);
        bars.moveTo(12.0, 18.6);
        bars.lineTo(12.0, 5.4);
        bars.moveTo(17.4, 18.6);
        bars.lineTo(17.4, 9.4);
        strokeIn(p, bars, color, 2.7);
        break;
    }
    }
    p.restore();
}

QColor hue(Glyph glyph)
{
    switch (glyph) {
    case Glyph::Photo:
        return QColor(0x3b, 0x82, 0xf6); // blue: 3.7:1 on white, 4.0:1 on #2b2d31
    case Glyph::Document:
        return QColor(0x8b, 0x5c, 0xf6); // violet
    case Glyph::Poll:
        return QColor(0x1f, 0xa2, 0x5a); // green
    case Glyph::Paperclip:
        break;
    }
    return QColor(0x85, 0x89, 0x91);
}

QIcon icon(Glyph glyph)
{
    QIcon out;
    const QColor color = hue(glyph);
    for (const int px : {24, 30, 36, 48, 72}) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        draw(p, QRectF(0, 0, px, px), glyph, color);
        p.end();
        out.addPixmap(pm);
    }
    return out;
}

} // namespace attachart
