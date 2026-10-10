#include "replyart.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

#include "emojitext.h" // 2.2 emoji: HD emoji in names and snippets
#include "uiutil.h"

namespace replyart {

namespace {

// The reply line: a connector from the message icon below (TeamSpeak's 13 px icon starts each message
// line, so its centre is at 6.5) up and to the right, then the name and the snippet.
constexpr qreal kSpineX    = 6.5;
constexpr qreal kSpineEnd  = 19.0;
constexpr qreal kContentX  = 24.0;
constexpr qreal kGap       = 5.0;
constexpr qreal kClipBox   = 12.0;
constexpr qreal kTextScale = 0.92;

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}

QColor baseOr(bool dark, const QColor& base)
{
    return base.isValid() && base.alpha() == 255 ? base : (dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff));
}

struct Fonts {
    QFont         name;
    QFont         snippet;
    QFontMetricsF nameMetrics;
    QFontMetricsF snippetMetrics;
};

Fonts fontsFor(const HeaderStyle& style, bool underline)
{
    QFont name = scaledFont(style.font, kTextScale);
    name.setWeight(QFont::DemiBold);
    name.setUnderline(underline);
    QFont snippet = scaledFont(style.font, kTextScale);
    snippet.setUnderline(false);
    return {name, snippet, QFontMetricsF(name), QFontMetricsF(snippet)};
}

qreal lineHeightOf(const HeaderStyle& style)
{
    if (style.lineHeight > 0)
        return std::ceil(style.lineHeight);
    return std::ceil(QFontMetricsF(style.font).height());
}

// What fits: the name (shortened when it would leave the snippet no room) and the snippet.
struct Fit {
    QString name;
    qreal   nameWidth = 0;
    QString snippet;
    qreal   snippetX  = 0; // where the clip glyph (files) or the snippet starts
};

Fit fitText(const Header& header, const Fonts& fonts, qreal width)
{
    Fit         fit;
    const qreal avail   = qMax(0.0, width - kContentX - 2.0);
    const qreal extra   = header.media ? kClipBox + 3.0 : 0.0;
    const bool  hasText = !header.snippet.isEmpty();
    qreal       nameW   = fonts.nameMetrics.horizontalAdvance(header.nick);
    if (hasText && avail - nameW - kGap - extra < 48.0)
        nameW = qMin(nameW, qMax(avail * 0.4, avail - kGap - extra - 48.0));
    nameW         = qMin(nameW, avail);
    fit.name      = fonts.nameMetrics.elidedText(header.nick, Qt::ElideRight, nameW);
    fit.nameWidth = fonts.nameMetrics.horizontalAdvance(fit.name);
    fit.snippetX  = kContentX + fit.nameWidth + kGap;
    if (hasText) {
        const qreal room = avail - fit.nameWidth - kGap - extra;
        if (room >= 16.0)
            fit.snippet = fonts.snippetMetrics.elidedText(header.snippet, Qt::ElideRight, room);
    }
    return fit;
}

QPointF at(const QRectF& box, qreal x, qreal y)
{
    return QPointF(box.left() + x * box.width(), box.top() + y * box.height());
}

QRectF unit(const QRectF& box, qreal x, qreal y, qreal w, qreal h)
{
    return QRectF(box.left() + x * box.width(), box.top() + y * box.height(), w * box.width(), h * box.height());
}

} // namespace

QColor readable(const QColor& color, const QColor& base, const QColor& fallback, double minimum)
{
    QColor c = color.isValid() ? color : fallback;
    c.setAlpha(255);
    const QColor bg     = base.isValid() ? base : QColor(Qt::white);
    const QColor target = bg.lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    for (int step = 0; step < 12 && ui::contrastRatio(c, bg) < minimum; ++step)
        c = mix(c, target, 0.15);
    return c;
}

Palette paletteFor(bool dark, const QColor& base)
{
    const QColor b = baseOr(dark, base);
    Palette      p;
    p.text    = readable(dark ? QColor(0xdb, 0xde, 0xe1) : QColor(0x31, 0x33, 0x38), b, QColor(), 7.0);
    p.muted   = readable(dark ? QColor(0xb5, 0xba, 0xc1) : QColor(0x5c, 0x5e, 0x66), b, QColor(), 4.5);
    p.spine   = dark ? QColor(0x6a, 0x6e, 0x79) : QColor(0xa8, 0xad, 0xb5);
    p.accent  = dark ? QColor(0x79, 0x84, 0xf5) : QColor(0x58, 0x65, 0xf2);
    p.name    = readable(dark ? QColor(0xc9, 0xcd, 0xfb) : QColor(0x47, 0x52, 0xc4), b, QColor(), 4.5);
    p.surface = ui::flatten(dark ? QColor(255, 255, 255, 13) : QColor(0, 0, 0, 10), b);
    p.hover   = ui::flatten(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 16), b);
    return p;
}

QFont scaledFont(const QFont& chatFont, qreal factor)
{
    QFont font(chatFont);
    qreal px = chatFont.pixelSize() > 0 ? chatFont.pixelSize() : (chatFont.pointSizeF() > 0 ? chatFont.pointSizeF() * 96.0 / 72.0 : 12.0);
    px       = qBound(11.0, px, 18.0);
    font.setPixelSize(qMax(10, qRound(px * factor)));
    return font;
}

void drawRun(QPainter& p, qreal x, qreal baseline, const QFontMetricsF& fm, const QString& text)
{
    // 2.2 emoji: the table's emoji as HD pictures in their glyphs' places (same widths and direction).
    emoji::drawTextLine(p, x, baseline, fm, text, text.isRightToLeft());
}

QColor headerNameColor(const Header& header, const HeaderStyle& style)
{
    const Palette pal = paletteFor(style.dark, style.base);
    return header.found ? readable(header.nickColor, baseOr(style.dark, style.base), pal.name) : pal.muted;
}

QSize headerSize(const Header& header, const HeaderStyle& style, int maxWidth)
{
    const Fonts fonts   = fontsFor(style, false);
    qreal       natural = kContentX + fonts.nameMetrics.horizontalAdvance(header.nick) + 3.0;
    if (!header.snippet.isEmpty())
        natural += kGap + (header.media ? kClipBox + 3.0 : 0.0) + fonts.snippetMetrics.horizontalAdvance(header.snippet);
    const int width = qMax(qMin(48, maxWidth), qMin(maxWidth, static_cast<int>(std::ceil(natural))));
    return QSize(qMax(1, width), qMax(8, static_cast<int>(lineHeightOf(style))));
}

QRectF headerTargetRect(const Header& header, const HeaderStyle& style, const QSize& size)
{
    Q_UNUSED(header);
    Q_UNUSED(style);
    return QRectF(QPointF(0, 0), QSizeF(size));
}

QImage renderHeader(const Header& header, const HeaderStyle& style, const QSize& size)
{
    const qreal dpr = style.dpr > 0 ? style.dpr : 1.0;
    QImage      img(qMax(1, qRound(size.width() * dpr)), qMax(1, qRound(size.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // The line reads left to right (connector, name, snippet); a Persian name or snippet keeps its own
    // order inside its place (with the default, Auto, right-to-left text would start at the right).
    p.setLayoutDirection(Qt::LeftToRight);

    const Palette pal   = paletteFor(style.dark, style.base);
    const bool    live  = header.found && (style.hovered || style.pressed);
    const Fonts   fonts = fontsFor(style, live);
    const qreal   h     = size.height();
    const qreal   base  = std::round((h + fonts.nameMetrics.ascent() - fonts.nameMetrics.descent()) / 2.0);
    const qreal   mid   = std::round(base - fonts.nameMetrics.xHeight() / 2.0) + 0.5;
    const qreal   r     = qMax(2.0, qMin(5.0, h - mid - 1.0));

    // The connector: up from the bottom edge (towards the message's icon), round corner, to the right.
    QPainterPath spine;
    spine.moveTo(kSpineX, h + 2.0);
    spine.lineTo(kSpineX, mid + r);
    spine.quadTo(kSpineX, mid, kSpineX + r, mid);
    spine.lineTo(kSpineEnd, mid);
    p.setPen(QPen(live ? pal.muted : pal.spine, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(spine);

    const Fit fit = fitText(header, fonts, size.width());
    p.setFont(fonts.name);
    p.setPen(headerNameColor(header, style));
    drawRun(p, kContentX, base, fonts.nameMetrics, fit.name);

    if (!fit.snippet.isEmpty()) {
        const QColor snippetColor = live ? pal.text : pal.muted;
        qreal        x            = fit.snippetX;
        if (header.media) {
            drawGlyph(p, Glyph::Clip, QRectF(x, mid - kClipBox / 2.0, kClipBox, kClipBox), snippetColor);
            x += kClipBox + 3.0;
        }
        p.setFont(fonts.snippet);
        p.setPen(snippetColor);
        drawRun(p, x, base, fonts.snippetMetrics, fit.snippet);
    }
    p.end();
    return img;
}

// ---- glyphs ------------------------------------------------------------------------------------------

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    const qreal s = qMin(box.width(), box.height());
    const QRectF b(box.center().x() - s / 2.0, box.center().y() - s / 2.0, s, s);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    QPen pen(color, qMax(1.2, s * 0.1), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    switch (glyph) {
    case Glyph::Reply: {
        QPainterPath head;
        head.moveTo(at(b, 0.42, 0.20));
        head.lineTo(at(b, 0.17, 0.44));
        head.lineTo(at(b, 0.42, 0.68));
        p.drawPath(head);
        QPainterPath shaft;
        shaft.moveTo(at(b, 0.19, 0.44));
        shaft.lineTo(at(b, 0.56, 0.44));
        shaft.cubicTo(at(b, 0.76, 0.44), at(b, 0.85, 0.56), at(b, 0.85, 0.80));
        p.drawPath(shaft);
        break;
    }
    case Glyph::Close:
        p.drawLine(at(b, 0.29, 0.29), at(b, 0.71, 0.71));
        p.drawLine(at(b, 0.71, 0.29), at(b, 0.29, 0.71));
        break;
    case Glyph::Clip: {
        pen.setWidthF(qMax(1.1, s * 0.09));
        p.setPen(pen);
        p.translate(b.center());
        p.rotate(40.0);
        p.translate(-b.center());
        QPainterPath clip;
        clip.moveTo(at(b, 0.62, 0.30));
        clip.lineTo(at(b, 0.62, 0.68));
        clip.arcTo(unit(b, 0.38, 0.56, 0.24, 0.24), 0.0, -180.0);
        clip.lineTo(at(b, 0.38, 0.27));
        clip.arcTo(unit(b, 0.38, 0.17, 0.19, 0.19), 180.0, -180.0);
        clip.lineTo(at(b, 0.57, 0.63));
        p.drawPath(clip);
        break;
    }
    case Glyph::Replies: {
        pen.setWidthF(qMax(1.1, s * 0.09));
        p.setPen(pen);
        QPainterPath bubble;
        bubble.addRoundedRect(unit(b, 0.14, 0.17, 0.72, 0.50), s * 0.13, s * 0.13);
        QPainterPath tail;
        tail.moveTo(at(b, 0.30, 0.67));
        tail.lineTo(at(b, 0.25, 0.85));
        tail.lineTo(at(b, 0.47, 0.67));
        p.drawPath(bubble.united(tail));
        p.drawLine(at(b, 0.31, 0.36), at(b, 0.69, 0.36));
        p.drawLine(at(b, 0.31, 0.49), at(b, 0.57, 0.49));
        break;
    }
    case Glyph::Jump: {
        p.drawLine(at(b, 0.30, 0.70), at(b, 0.70, 0.30));
        QPainterPath head;
        head.moveTo(at(b, 0.38, 0.30));
        head.lineTo(at(b, 0.70, 0.30));
        head.lineTo(at(b, 0.70, 0.62));
        p.drawPath(head);
        break;
    }
    }
    p.restore();
}

QIcon glyphIcon(Glyph glyph, const QColor& color, qreal dpr, int size)
{
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap     pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    drawGlyph(p, glyph, QRectF(0, 0, size, size).adjusted(1, 1, -1, -1), color);
    p.end();
    return QIcon(pixmap);
}

} // namespace replyart
