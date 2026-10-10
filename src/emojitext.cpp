#include "emojitext.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
#include <QVector>

#include <cmath>

#include "emojidata.h"
#include "emojirender.h"
#include "emojisegment.h"

namespace emoji {

namespace {

constexpr int    kMaxPictures   = 24;  // per line: a snippet of hundreds of emoji stays cheap to draw
constexpr qint64 kBudgetWindowMs = 250; // setNewPictureBudget(): per this long

bool          g_pictures      = true;
qint64        g_budgetNs      = -1; // < 0: no budget
qint64        g_spentNs       = 0;
QElapsedTimer g_window;
int           g_textFallbacks = 0;

qreal snapped(qreal value, qreal dpr)
{
    return std::round(value * dpr) / dpr;
}

// The picture of emoji id: cached, drawn now (within the budget), or asked of the worker (null: its
// glyph stays this time).
QImage pictureFor(int id, int side, qreal dpr)
{
    QImage image = cachedImage(text(id), side, dpr);
    if (!image.isNull() || g_budgetNs < 0)
        return image.isNull() ? render(id, side, dpr) : image;
    if (!g_window.isValid() || g_window.elapsed() >= kBudgetWindowMs) {
        g_window.start();
        g_spentNs = 0;
    }
    if (g_spentNs < g_budgetNs) {
        QElapsedTimer clock;
        clock.start();
        image = render(id, side, dpr);
        g_spentNs += clock.nsecsElapsed();
        return image;
    }
    image = requestImage(id, side, dpr, true);
    if (image.isNull())
        ++g_textFallbacks;
    return image;
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

void setNewPictureBudget(int ms)
{
    g_budgetNs = ms < 0 ? -1 : static_cast<qint64>(ms) * 1000000;
    g_spentNs  = 0;
    g_window.invalidate();
}

int takeTextFallbacks()
{
    const int count = g_textFallbacks;
    g_textFallbacks = 0;
    return count;
}

void drawTextLine(QPainter& p, qreal x, qreal baseline, const QFontMetricsF& fm, const QString& text, bool rightToLeft)
{
    if (text.isEmpty())
        return;
    const QRectF box(x, baseline - fm.ascent(), fm.horizontalAdvance(text) + 2.0, fm.ascent() + fm.descent());
    const int    flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine | (rightToLeft ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight);

    QVector<Match> found;
    if (g_pictures && hasColor()) {
        for (const Match& m : findEmoji(text, kMaxPictures)) {
            if (showsAsPicture(m) && supported(m.id))
                found.append(m);
        }
    }
    if (found.isEmpty()) {
        p.drawText(box, flags, text);
        return;
    }

    // One line laid out like drawText's. First where each emoji's glyph is, then the line again with the
    // glyphs that get a picture transparent (they keep their room; colours move no glyph).
    QTextOption option(Qt::AlignLeft | Qt::AlignAbsolute);
    option.setTextDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
    option.setWrapMode(QTextOption::NoWrap);
    const auto lineOf = [&](QTextLayout& layout) {
        layout.setTextOption(option);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (line.isValid()) {
            line.setLineWidth(box.width());
            line.setPosition(QPointF(0, 0));
        }
        layout.endLayout();
        return line;
    };
    QTextLayout     measure(text, p.font());
    const QTextLine probe = lineOf(measure);
    if (!probe.isValid()) {
        p.drawText(box, flags, text);
        return;
    }

    // The pictures where the glyphs are: as wide as the glyph, never taller than the text.
    struct Picture {
        Match  match;
        qreal  left = 0;
        int    side = 0;
        QImage image;
    };
    const qreal      dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    QVector<Picture> pictures;
    for (const Match& m : qAsConst(found)) {
        const qreal a     = probe.cursorToX(m.start);
        const qreal b     = probe.cursorToX(m.start + m.length);
        const qreal width = std::abs(b - a);
        const int   side  = static_cast<int>(std::floor(qMin(width, fm.ascent() + fm.descent())));
        if (side < 6)
            continue;
        const QImage image = pictureFor(m.id, side, dpr);
        if (!image.isNull())
            pictures.append({m, x + qMin(a, b) + (width - side) / 2.0, side, image});
    }
    if (pictures.isEmpty()) {
        p.drawText(box, flags, text);
        return;
    }

    QTextLayout                       layout(text, p.font());
    QVector<QTextLayout::FormatRange> ranges;
    for (const Picture& picture : qAsConst(pictures)) {
        QTextLayout::FormatRange range;
        range.start  = picture.match.start;
        range.length = picture.match.length;
        range.format.setForeground(QColor(Qt::transparent));
        ranges.append(range);
    }
    layout.setFormats(ranges);
    const QTextLine line = lineOf(layout);
    if (!line.isValid()) {
        p.drawText(box, flags, text);
        return;
    }
    layout.draw(&p, QPointF(x, baseline - line.ascent()));

    const qreal top    = baseline - fm.ascent();
    const bool  smooth = p.testRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const Picture& picture : qAsConst(pictures)) {
        const qreal y = snapped(top + (fm.ascent() + fm.descent() - picture.side) / 2.0, dpr);
        p.drawImage(QRectF(snapped(picture.left, dpr), y, picture.side, picture.side), picture.image);
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
}

} // namespace emoji
