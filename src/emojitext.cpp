#include "emojitext.h"

#include <QFontMetricsF>
#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
#include <QVector>

#include <cmath>

#include "emojirender.h"
#include "emojisegment.h"

namespace emoji {

namespace {

constexpr int kMaxPictures = 24; // per line: a snippet of hundreds of emoji stays cheap to draw

bool g_pictures = true;

qreal snapped(qreal value, qreal dpr)
{
    return std::round(value * dpr) / dpr;
}

} // namespace

void setTextPicturesEnabled(bool enabled)
{
    g_pictures = enabled;
}

bool textPicturesEnabled()
{
    return g_pictures;
}

void drawTextLine(QPainter& p, qreal x, qreal baseline, const QFontMetricsF& fm, const QString& text, bool rightToLeft)
{
    if (text.isEmpty())
        return;
    const QRectF box(x, baseline - fm.ascent(), fm.horizontalAdvance(text) + 2.0, fm.ascent() + fm.descent());
    const int    flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine | (rightToLeft ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight);

    QVector<Match> pictures;
    if (g_pictures && hasColor()) {
        for (const Match& m : findEmoji(text, kMaxPictures)) {
            if (showsAsPicture(m) && supported(m.id))
                pictures.append(m);
        }
    }
    if (pictures.isEmpty()) {
        p.drawText(box, flags, text);
        return;
    }

    // One line laid out like drawText's, the emoji's glyphs transparent: they keep their room.
    QTextLayout layout(text, p.font());
    QTextOption option(Qt::AlignLeft | Qt::AlignAbsolute);
    option.setTextDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    QVector<QTextLayout::FormatRange> ranges;
    for (const Match& m : qAsConst(pictures)) {
        QTextLayout::FormatRange range;
        range.start  = m.start;
        range.length = m.length;
        range.format.setForeground(QColor(Qt::transparent));
        ranges.append(range);
    }
    layout.setFormats(ranges);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        p.drawText(box, flags, text);
        return;
    }
    line.setLineWidth(box.width());
    line.setPosition(QPointF(0, 0));
    layout.endLayout();
    layout.draw(&p, QPointF(x, baseline - line.ascent()));

    // The pictures where the glyphs would be: as wide as the glyph, never taller than the text.
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const qreal top = baseline - fm.ascent();
    const bool  smooth = p.testRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const Match& m : qAsConst(pictures)) {
        const qreal a     = line.cursorToX(m.start);
        const qreal b     = line.cursorToX(m.start + m.length);
        const qreal width = std::abs(b - a);
        const int   side  = static_cast<int>(std::floor(qMin(width, fm.ascent() + fm.descent())));
        if (side < 6)
            continue;
        const QImage image = render(m.id, side, dpr);
        if (image.isNull())
            continue;
        const qreal left = snapped(x + qMin(a, b) + (width - side) / 2.0, dpr);
        const qreal y    = snapped(top + (fm.ascent() + fm.descent() - side) / 2.0, dpr);
        p.drawImage(QRectF(left, y, side, side), image);
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
}

} // namespace emoji
