#pragma once

// 2.5 zoom (design2.md 1.2-1.4): previews follow the chat's text size, Windows scaling and the "Preview size"
// setting. Every drawing constant stays as it was, in design units (du: the logical pixels of 2.4). A chat has one
// preview scale s (from its own text size) and one chrome scale c (reply lines, reaction rows under text; the same
// formula without the preset). Art modules lay out in du and draw on canvases of ratio R = min(dpr * s, cap);
// sizes leaving them are multiplied by s, pointer positions entering them divided by s. This header is the one
// place that converts. Header only and pure (the tests use it as it is); integer arithmetic, so every client and
// test gets the same numbers.

#include <QFont>
#include <QFontInfo>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextDocument>
#include <QTextFormat>
#include <QtGlobal>
#include <QtMath>

namespace previewscale {

// The largest canvas ratio (one 400x420 du card at R 4 is about 11 MB of pixels). The 32-bit build shares its
// address space with TeamSpeak and its Qt: 3 there. Beyond the cap Qt upscales slightly.
constexpr qreal kMaxRenderRatio = sizeof(void*) == 8 ? 4.0 : 3.0;
// GIF and loop frames (QMovie decodes them on the GUI thread) are decoded at most at this ratio, and never above
// their natural size: drawing scales them to their box (design2 1.7).
constexpr qreal kMaxAnimationRatio = 2.0;
// The "Preview size" presets in percent (Small, Normal, Large, Larger).
constexpr int kPresets[]      = {85, 100, 125, 150};
constexpr int kDefaultPreset  = 100;
// The du size (QSizeF) of a picture object of ours, next to its logical width and height: sizes are compared in du,
// never as converted logical sizes (no flip-flop from rounding). Every module's objects carry it.
constexpr int kDuSizeProperty = QTextFormat::UserProperty + 0x74e0;
// Media in the chat: the 48 du ChatIntegration keeps free right of a picture (the 2.4 "previewAreaWidth - 48").
constexpr int kMediaMargin = 48;
// Every look's smallest width.
constexpr int kMinWidth = 160;

// A valid preset: the nearest of kPresets (ties go down).
inline int nearestPreset(int value)
{
    int best = kPresets[0];
    for (const int p : kPresets) {
        if (qAbs(p - value) < qAbs(best - value))
            best = p;
    }
    return best;
}

} // namespace previewscale

struct PreviewScale {
    int   s20      = 20; // preview scale x 20 (0.50 .. 3.00 in steps of 0.05)
    int   c20      = 20; // chrome scale x 20
    qreal dpr      = 1.0; // the chat's devicePixelRatioF()
    qreal maxRatio = previewscale::kMaxRenderRatio;

    qreal s() const { return s20 / 20.0; }
    qreal c() const { return c20 / 20.0; }
    // R: the ratio previews are drawn at (canvas pixels per du), 1 .. maxRatio. Never below 1: QPainter takes a paint
    // device's ratio as at least 1 (QPainterPrivate::effectiveDevicePixelRatio), so a canvas of ratio 0.85 would be
    // drawn unscaled and cut off. Below 1 (Small, a zoomed-out chat) the picture is drawn at 1 and made smaller,
    // smoothly, for the document (forDocument() below).
    qreal ratio() const { return qBound<qreal>(1.0, displayRatio(), maxRatio); }
    // The ratio for chrome (reply lines, reaction rows under text): c instead of s.
    qreal chromeRatio() const { return qBound<qreal>(1.0, (dpr > 0 ? dpr : 1.0) * c(), maxRatio); }
    // Device pixels per du on the screen (dpr x s): what a picture needs to be shown 1:1.
    qreal displayRatio() const { return (dpr > 0 ? dpr : 1.0) * s(); }

    // A logical length on whole device pixels (lines under an object never sit at sub-pixel offsets).
    qreal snap(qreal logical) const
    {
        const qreal d = dpr > 0 ? dpr : 1.0;
        return qRound(logical * d) / d;
    }
    qreal  toLogical(qreal du) const { return snap(du * s()); }
    QSizeF toLogical(const QSizeF& du) const { return QSizeF(toLogical(du.width()), toLogical(du.height())); }
    QRectF toLogical(const QRectF& du) const
    {
        const qreal left = snap(du.left() * s()), top = snap(du.top() * s());
        return QRectF(left, top, snap(du.right() * s()) - left, snap(du.bottom() * s()) - top);
    }
    // Chrome (c) the same way.
    qreal  chromeToLogical(qreal du) const { return snap(du * c()); }
    QSizeF chromeToLogical(const QSizeF& du) const { return QSizeF(chromeToLogical(du.width()), chromeToLogical(du.height())); }

    QPointF toDu(const QPointF& logical) const { return logical / s(); }
    QRectF  toDu(const QRectF& logical) const { return QRectF(logical.topLeft() / s(), logical.size() / s()); }
    QSizeF  toDu(const QSizeF& logical) const { return logical / s(); }
    // Whole du that fit into a logical length (a column, a margin).
    int duFromLogical(qreal logical) const { return qFloor(logical / s() + 1e-6); }
    int chromeDuFromLogical(qreal logical) const { return qFloor(logical / c() + 1e-6); }

    // The chrome scale as a scale of its own (s = c): reply lines and reaction rows under text use the object helpers
    // below with it.
    PreviewScale chrome() const
    {
        PreviewScale out = *this;
        out.s20          = c20;
        return out;
    }

    // The same look (sizes and canvas ratio): nothing needs laying out or drawing again.
    bool sameAs(const PreviewScale& o) const { return s20 == o.s20 && c20 == o.c20 && qFuzzyCompare(ratio(), o.ratio()); }
    // For render signatures and keys.
    QString key() const { return QString::number(s20) + QLatin1Char('/') + QString::number(c20) + QLatin1Char('/') + QString::number(ratio(), 'f', 4); }
};

namespace previewscale {

// design2 1.2 (normative): the chat's scales from its text size (chatPx: QFontInfo of the document's default font),
// the preset, the column a preview may use (columnL = previewAreaWidth - the chat's indent), the document margin and
// the bubble's right margin (0 off bubbles), all logical px. s20 = min(font scale, fit), fit keeping every look's
// 160 du floor and its margins inside the column: media (160 + 48) du; links and polls (160 + 2) du plus the bubble
// margin, which follows the chat font.
// The font part alone (x 20): the preset times the chat's text size, before any column fit. 2.5 YouTube player: the
// popup is a window of its own, so a narrow chat's fit doesn't make it (and YouTube's page in it) smaller.
inline int fontScale20(int chatPx, int presetPct)
{
    return qBound(15, qMax(1, chatPx) * presetPct / 55, 60);
}

inline PreviewScale computeScale(int chatPx, int presetPct, qreal columnL, qreal docL, int bubbleL, qreal dpr)
{
    const int    px       = qMax(1, chatPx);
    const qreal  inner    = columnL - 2.0 * docL;
    const int    media    = qFloor(20.0 * inner / (kMinWidth + kMediaMargin) + 1e-9);
    const int    links    = qFloor(20.0 * (inner - qMax(0, bubbleL)) / (kMinWidth + 2) + 1e-9);
    const int    fit20    = qMax(10, qMin(media, links));
    PreviewScale out;
    out.s20 = qMin(fontScale20(px, presetPct), fit20);
    out.c20 = qMin(qBound(15, px * 100 / 55, 60), fit20);
    out.dpr = dpr > 0 ? dpr : 1.0;
    return out;
}

// The chat's text size in logical px (ChatLayout's f): QFontInfo of the document's default font.
inline int chatPxOf(const QTextDocument* doc)
{
    if (!doc)
        return 11;
    return qMax(1, QFontInfo(doc->defaultFont()).pixelSize());
}

// The widest uploaded media in du for a chat (design2 1.5): the setting at Normal size and 100%, within the column
// less the 48 du margin, never under 160 du. areaL: previewAreaWidth, indentL: the block's indent (logical px).
inline int mediaMaxWidthDu(int previewMaxWidth, qreal areaL, qreal indentL, const PreviewScale& scale)
{
    // The widest du whose logical width (on whole device px) still fits; s 1: the 2.4 "area - 48 - indent".
    const qreal avail = areaL - indentL - kMediaMargin * scale.s();
    int         du    = qFloor(avail / scale.s() + 1e-6);
    while (du > kMinWidth && scale.toLogical(du) > avail + 1e-6)
        --du;
    return qMin(previewMaxWidth, qMax(kMinWidth, du));
}

// The widest link card or poll in du (design2 1.5): within the column less the document margins and the right margin
// (12 du off bubbles; on a bubble its right edge, which follows the chat font, plus 2 du), never under 160 du.
// designMax: the look's own limit before the column (the uploaded media's maxWidth for links; none for polls).
inline int cardMaxWidthDu(int designMax, qreal areaL, qreal indentL, qreal docL, int bubbleRightL, const PreviewScale& scale)
{
    // In logical px: the right margin is 12 du, or on a bubble its right edge (the chat font's) plus 2 du. The widest du
    // whose logical width (on whole device px) still fits; s 1: the 2.4 "area - indent - 2 margins - max(12, right + 2)".
    const qreal s     = scale.s();
    const qreal right = qMax(12.0 * s, bubbleRightL > 0 ? bubbleRightL + 2.0 * s : 0.0);
    const qreal avail = areaL - indentL - 2.0 * docL - right;
    int         du    = qFloor(avail / s + 1e-6);
    while (du > kMinWidth && scale.toLogical(du) > avail + 1e-6)
        --du;
    return qMax(kMinWidth, qMin(designMax, du));
}

// ---- objects in the document --------------------------------------------------------------------------------------

// A picture object's size for du at scale: its logical width and height (whole device px) and the du property.
inline void setObjectSize(QTextImageFormat& format, const QSizeF& du, const PreviewScale& scale)
{
    const QSizeF logical = scale.toLogical(du);
    format.setWidth(logical.width());
    format.setHeight(logical.height());
    format.setProperty(kDuSizeProperty, du);
}

// The same for chrome (c).
inline void setChromeObjectSize(QTextImageFormat& format, const QSizeF& du, const PreviewScale& scale)
{
    const QSizeF logical = scale.chromeToLogical(du);
    format.setWidth(logical.width());
    format.setHeight(logical.height());
    format.setProperty(kDuSizeProperty, du);
}

// A picture for the document: one drawn at a ratio above what the screen shows it at (ratio() is never below 1, the
// screen's may be: Small, a zoomed-out chat) is made smaller, smoothly, to the screen's pixels (else the chat would
// shrink it with no smoothing). Anything else as it is. The document draws it into the object's logical size.
inline QImage forDocument(const QImage& drawn, qreal displayRatio)
{
    const qreal have = drawn.devicePixelRatio() > 0 ? drawn.devicePixelRatio() : 1.0;
    if (drawn.isNull() || displayRatio <= 0 || displayRatio >= have - 0.001)
        return drawn;
    const QSize want(qMax(1, qRound(drawn.width() * displayRatio / have)), qMax(1, qRound(drawn.height() * displayRatio / have)));
    QImage      out = drawn.scaled(want, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    out.setDevicePixelRatio(displayRatio);
    return out;
}

// The du size a picture object was laid out for; one without the property (from before 2.5's zoom): its logical size.
inline QSizeF duSizeOf(const QTextFormat& format)
{
    const QVariant v = format.property(kDuSizeProperty);
    if (v.isValid() && v.type() == QVariant::SizeF)
        return v.toSizeF();
    const QTextImageFormat image = format.toImageFormat();
    return QSizeF(image.width(), image.height());
}

// The decode size of a GIF or loop frame (design2 1.7): the box in device px at the ratio, capped at
// kMaxAnimationRatio, and never above the frame's natural size (drawing scales it to the box). Aspect kept.
inline QSize animationFrameSize(const QSize& natural, const QSizeF& boxDu, qreal ratio)
{
    if (natural.isEmpty() || boxDu.isEmpty())
        return natural;
    const qreal r   = qMin(ratio > 0 ? ratio : 1.0, kMaxAnimationRatio);
    const QSize box = QSize(qMax(1, qRound(boxDu.width() * r)), qMax(1, qRound(boxDu.height() * r)));
    if (natural.width() <= box.width() && natural.height() <= box.height())
        return natural;
    return natural.scaled(box, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

} // namespace previewscale
