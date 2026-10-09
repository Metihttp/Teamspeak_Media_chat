#include "dragpixmap.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include "i18n.h"
#include "previewrenderer.h"

namespace {

constexpr qreal kThumbBox     = 160.0;
constexpr qreal kRadius       = 8.0;
constexpr qreal kOpacity      = 0.85;
constexpr int   kCardWidth    = 240;
constexpr int   kCardHeight   = 56;
constexpr qreal kCardPadding  = 12.0;
constexpr qreal kGlyphWidth   = 32.0;
constexpr qreal kGlyphHeight  = 40.0;

QImage canvas(const QSize& logical, qreal dpr)
{
    QImage out(QSize(qMax(1, qRound(logical.width() * dpr)), qMax(1, qRound(logical.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    return out;
}

// Pixel sizes like the chat previews (previewrenderer.cpp): independent of the screen's DPI.
QFont sized(const QFont& base, qreal pixels, bool bold)
{
    QFont font(base);
    font.setPixelSize(qMax(8, qRound(pixels)));
    font.setBold(bold);
    font.setItalic(false);
    font.setUnderline(false);
    return font;
}

qreal basePixels(const QFont& font)
{
    qreal px = 12.0;
    if (font.pixelSize() > 0)
        px = font.pixelSize();
    else if (font.pointSizeF() > 0)
        px = font.pointSizeF() * 4.0 / 3.0;
    return qBound(11.0, px, 15.0);
}

} // namespace

QColor dragCardBackground()
{
    return QColor(0x2b, 0x2d, 0x31);
}

QColor dragCardTitleColor()
{
    return QColor(0xf2, 0xf3, 0xf5);
}

QColor dragCardDetailColor()
{
    return QColor(0xb5, 0xba, 0xc1);
}

DragImage renderDragPicture(const QImage& still, qreal dpr, const QPointF& pressFraction)
{
    DragImage result;
    if (still.isNull() || still.width() <= 0 || still.height() <= 0)
        return result;
    dpr = qBound(0.5, dpr > 0 ? dpr : 1.0, 8.0);

    // Never larger than the still itself (a tiny emoji stays tiny), at least 24 px so it is seen.
    // Stills from Core are device pixels for this dpr.
    const QSizeF stillLogical = QSizeF(still.size()) / dpr;
    QSizeF       size         = stillLogical.scaled(QSizeF(kThumbBox, kThumbBox), Qt::KeepAspectRatio);
    if (stillLogical.width() <= kThumbBox && stillLogical.height() <= kThumbBox)
        size = stillLogical;
    size = size.expandedTo(QSizeF(24, 24));
    const QSize logical(qMax(1, qRound(size.width())), qMax(1, qRound(size.height())));

    QImage   out = canvas(logical, dpr);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(kOpacity);
    const QRectF bounds(QPointF(0, 0), QSizeF(logical));
    QPainterPath clip;
    clip.addRoundedRect(bounds, kRadius, kRadius);
    p.setClipPath(clip);
    p.drawImage(bounds, still);
    p.setClipping(false);
    p.setPen(QPen(QColor(0, 0, 0, 64), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), kRadius - 0.5, kRadius - 0.5);
    p.end();

    result.image = out;
    const qreal fx = qBound(0.0, pressFraction.x(), 1.0);
    const qreal fy = qBound(0.0, pressFraction.y(), 1.0);
    result.hotSpot = QPoint(qBound(0, qRound(fx * logical.width()), logical.width() - 1), qBound(0, qRound(fy * logical.height()), logical.height() - 1));
    return result;
}

DragImage renderDragCard(const MediaEntry& entry, bool program, const QFont& font, qreal dpr)
{
    dpr = qBound(0.5, dpr > 0 ? dpr : 1.0, 8.0);
    const QSize logical(kCardWidth, kCardHeight);
    QImage      out = canvas(logical, dpr);
    QPainter    p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setLayoutDirection(Qt::LeftToRight);

    const QRectF bounds(QPointF(0, 0), QSizeF(logical));
    p.setPen(QPen(QColor(0x1e, 0x1f, 0x22), 1));
    p.setBrush(dragCardBackground());
    p.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    PreviewStyle style;
    style.dark = true;
    style.font = font;
    style.dpr  = dpr;
    drawFileTypeGlyph(p, QRectF(kCardPadding, (kCardHeight - kGlyphHeight) / 2.0, kGlyphWidth, kGlyphHeight), entry, style);

    const qreal         base      = basePixels(font);
    const QFont         titleFont = sized(font, base, true);
    const QFont         subFont   = sized(font, base - 1.5, false);
    const QFontMetricsF titleFm(titleFont);
    const QFontMetricsF subFm(subFont);
    const qreal         left   = kCardPadding + kGlyphWidth + kCardPadding;
    const qreal         width  = kCardWidth - left - kCardPadding;
    const qreal         blockH = titleFm.height() + 2 + subFm.height();
    const qreal         top    = (kCardHeight - blockH) / 2.0;

    // The name comes from a chat link: displayNameFor() strips bidi and control characters; it keeps
    // its own reading direction, aligned left like the rest of the card.
    const QString name  = titleFm.elidedText(displayNameFor(entry.link), Qt::ElideMiddle, width);
    const int     align = Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute | (name.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight);
    p.setFont(titleFont);
    p.setPen(dragCardTitleColor());
    p.drawText(QRectF(left, top, width, titleFm.height()), align, name);

    QString detail = entry.link.size > 0 ? formatSize(entry.link.size) : QString();
    if (program)
        detail = detail.isEmpty() ? i18n::t("Program") : i18n::t("Program · %1").arg(detail);
    p.setFont(subFont);
    p.setPen(dragCardDetailColor());
    p.drawText(QRectF(left, top + titleFm.height() + 2, width, subFm.height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute,
               subFm.elidedText(detail, Qt::ElideRight, width));
    p.end();

    DragImage result;
    result.image   = out;
    result.hotSpot = QPoint(24, 28);
    return result;
}
