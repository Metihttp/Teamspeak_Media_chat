#include "previewrenderer.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>

#include "i18n.h"

namespace {

constexpr qreal kRadius         = 8.0;
constexpr int   kCardWidth      = 340;
constexpr int   kCardHeight     = 62;
constexpr qreal kMinImageWidth  = 120.0;
constexpr qreal kMinImageHeight = 40.0;
constexpr qreal kMinVideoWidth  = 240.0;
constexpr qreal kMinVideoHeight = 135.0;
constexpr qreal kMaxUpscale     = 2.0; // small pictures may grow this much to make a usable click target
constexpr qreal kPlayButton     = 56.0;

const QColor kAccent(0x58, 0x65, 0xf2);

struct Palette {
    QColor background, border, title, link, muted, error, track, placeholder, hairline;
};

Palette paletteFor(bool dark)
{
    if (dark)
        return {QColor(0x2b, 0x2d, 0x31), QColor(0x1e, 0x1f, 0x22), QColor(0xf2, 0xf3, 0xf5), QColor(0x00, 0xa8, 0xfc),
                QColor(0x94, 0x9b, 0xa4), QColor(0xf2, 0x3f, 0x43), QColor(0x4e, 0x50, 0x58), QColor(0x23, 0x24, 0x28),
                QColor(255, 255, 255, 22)};
    return {QColor(0xf2, 0xf3, 0xf5), QColor(0xdc, 0xde, 0xe2), QColor(0x06, 0x06, 0x07), QColor(0x00, 0x6c, 0xe7),
            QColor(0x5c, 0x5e, 0x66), QColor(0xda, 0x37, 0x3c), QColor(0xd4, 0xd7, 0xdc), QColor(0xe3, 0xe5, 0xe8),
            QColor(0, 0, 0, 26)};
}

// ---- fonts / canvas ---------------------------------------------------------------------------

// Everything is laid out in logical pixels, so fonts are sized in pixels too (independent of the
// paint device's DPI) and derived from the chat font.
qreal basePixelSize(const QFont& font)
{
    qreal px = 12.0;
    if (font.pixelSize() > 0)
        px = font.pixelSize();
    else if (font.pointSizeF() > 0)
        px = font.pointSizeF() * 4.0 / 3.0;
    return qBound(11.0, px, 15.0);
}

QFont fontFor(const PreviewStyle& style, qreal delta, bool bold = false)
{
    QFont f = style.font;
    f.setPixelSize(qMax(8, qRound(basePixelSize(style.font) + delta)));
    f.setBold(bold);
    f.setItalic(false);
    f.setUnderline(false);
    f.setStrikeOut(false);
    return f;
}

qreal ratioOf(const PreviewStyle& style)
{
    return style.dpr > 0.0 ? qBound(0.5, style.dpr, 8.0) : 1.0;
}

QImage makeCanvas(const QSize& logical, qreal dpr)
{
    QImage out(QSize(qMax(1, qRound(logical.width() * dpr)), qMax(1, qRound(logical.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    return out;
}

void preparePainter(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);
}

// ---- geometry ---------------------------------------------------------------------------------

QSizeF clampAspect(QSizeF s)
{
    if (!(s.width() > 0) || !(s.height() > 0))
        return {};
    s = s.boundedTo(QSizeF(16384, 16384));
    const qreal aspect = s.width() / s.height();
    if (aspect > 8.0)
        s.setHeight(s.width() / 8.0);
    else if (aspect < 0.125)
        s.setWidth(s.height() / 8.0);
    return s;
}

QSizeF linkSize(const MediaEntry& e)
{
    if (e.link.width > 0 && e.link.height > 0)
        return clampAspect(QSizeF(e.link.width, e.link.height));
    return {};
}

QRectF centered(const QSizeF& size, const QRectF& box)
{
    return QRectF(box.center().x() - size.width() / 2.0, box.center().y() - size.height() / 2.0, size.width(), size.height());
}

QRectF fitRect(const QSizeF& content, const QRectF& box)
{
    if (content.isEmpty())
        return box;
    return centered(content.scaled(box.size(), Qt::KeepAspectRatio), box);
}

QRectF coverRect(const QSizeF& content, const QRectF& box)
{
    if (content.isEmpty())
        return box;
    return centered(content.scaled(box.size(), Qt::KeepAspectRatioByExpanding), box);
}

struct MediaLayout {
    QSize  box;     // the preview's logical size
    QSizeF content; // preferred size of the picture inside it (centered)
};

// Pictures: scaled down to fit maxWidth x maxHeight; tiny ones grow a little and get padded to a
// minimum size. Videos: same fit, then letterboxed up to a minimum player size.
MediaLayout mediaLayout(QSizeF natural, const PreviewStyle& style, bool video)
{
    const qreal maxW = qMax(48, style.maxWidth);
    const qreal maxH = qMax(32, style.maxHeight);
    natural          = clampAspect(natural);
    if (natural.isEmpty())
        natural = QSizeF(16, 9);

    const qreal scale   = qMin(1.0, qMin(maxW / natural.width(), maxH / natural.height()));
    QSizeF      content = natural * scale;
    const qreal minW    = qMin(video ? kMinVideoWidth : kMinImageWidth, maxW);
    const qreal minH    = qMin(video ? kMinVideoHeight : kMinImageHeight, maxH);
    if (!video && (content.width() < minW || content.height() < minH)) {
        const qreal want = qMax(minW / content.width(), minH / content.height());
        const qreal grow = std::min({want, kMaxUpscale, maxW / content.width(), maxH / content.height()});
        if (grow > 1.0)
            content *= grow;
    }

    QSize box(qRound(qMax(content.width(), minW)), qRound(qMax(content.height(), minH)));
    box     = box.boundedTo(QSize(qRound(maxW), qRound(maxH))).expandedTo(QSize(1, 1));
    content = content.boundedTo(QSizeF(box));
    return {box, content};
}

QSize cardSize(const PreviewStyle& style)
{
    return QSize(qMin(kCardWidth, qMax(160, style.maxWidth)), kCardHeight);
}

// ---- icons (vector paths: crisp at any device pixel ratio, no fonts/emoji) ----------------------

QPointF at(const QRectF& box, qreal fx, qreal fy)
{
    return QPointF(box.left() + box.width() * fx, box.top() + box.height() * fy);
}

QPen iconPen(const QColor& color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void drawPlayIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    QPainterPath path;
    path.moveTo(at(box, 0.30, 0.18));
    path.lineTo(at(box, 0.84, 0.50));
    path.lineTo(at(box, 0.30, 0.82));
    path.closeSubpath();
    p.setPen(iconPen(color, box.width() * 0.10));
    p.setBrush(color);
    p.drawPath(path);
}

void drawPauseIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal w = box.width() * 0.17;
    const qreal r = w * 0.35;
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(at(box, 0.24, 0.18), QSizeF(w, box.height() * 0.64)), r, r);
    p.drawRoundedRect(QRectF(at(box, 0.59, 0.18), QSizeF(w, box.height() * 0.64)), r, r);
}

void drawArrowHead(QPainter& p, const QPointF& tip, const QPointF& direction, qreal size, const QColor& color)
{
    const qreal len = std::hypot(direction.x(), direction.y());
    if (len <= 0)
        return;
    const QPointF d(direction.x() / len, direction.y() / len);
    const QPointF n(-d.y(), d.x());
    QPainterPath head;
    head.moveTo(tip + d * size * 0.55);
    head.lineTo(tip - d * size * 0.45 + n * size * 0.6);
    head.lineTo(tip - d * size * 0.45 - n * size * 0.6);
    head.closeSubpath();
    p.setPen(iconPen(color, size * 0.18));
    p.setBrush(color);
    p.drawPath(head);
}

// Circular arrow (replay / retry): open circle with the arrow at the top pointing left (↺).
void drawCircularArrow(QPainter& p, const QRectF& box, const QColor& color, qreal stroke)
{
    const QRectF ring = box.adjusted(box.width() * 0.19, box.height() * 0.21, -box.width() * 0.19, -box.height() * 0.17);
    QPainterPath arc;
    arc.arcMoveTo(ring, 90.0);
    arc.arcTo(ring, 90.0, -280.0);
    p.setPen(iconPen(color, stroke));
    p.setBrush(Qt::NoBrush);
    p.drawPath(arc);
    drawArrowHead(p, QPointF(ring.center().x() - stroke * 0.3, ring.top()), QPointF(-1, 0), box.width() * 0.34, color);
}

void drawSpeakerIcon(QPainter& p, const QRectF& box, const QColor& color, bool muted)
{
    QPainterPath body;
    body.moveTo(at(box, 0.14, 0.38));
    body.lineTo(at(box, 0.30, 0.38));
    body.lineTo(at(box, 0.50, 0.20));
    body.lineTo(at(box, 0.50, 0.80));
    body.lineTo(at(box, 0.30, 0.62));
    body.lineTo(at(box, 0.14, 0.62));
    body.closeSubpath();
    p.setPen(iconPen(color, box.width() * 0.06));
    p.setBrush(color);
    p.drawPath(body);

    p.setBrush(Qt::NoBrush);
    p.setPen(iconPen(color, box.width() * 0.085));
    if (muted) {
        p.drawLine(at(box, 0.64, 0.38), at(box, 0.88, 0.62));
        p.drawLine(at(box, 0.88, 0.38), at(box, 0.64, 0.62));
        return;
    }
    const QPointF c = at(box, 0.50, 0.50);
    for (qreal r : {0.17, 0.32}) {
        const qreal  rr = box.width() * r;
        const QRectF arcBox(c.x() - rr, c.y() - rr, rr * 2, rr * 2);
        QPainterPath wave;
        wave.arcMoveTo(arcBox, -45);
        wave.arcTo(arcBox, -45, 90);
        p.drawPath(wave);
    }
}

void drawExpandIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    p.setPen(iconPen(color, box.width() * 0.09));
    p.setBrush(Qt::NoBrush);
    const qreal a = 0.18, b = 0.82, arm = 0.22;
    const QPointF corners[4][3] = {
        {at(box, a, a + arm), at(box, a, a), at(box, a + arm, a)},
        {at(box, b - arm, a), at(box, b, a), at(box, b, a + arm)},
        {at(box, b, b - arm), at(box, b, b), at(box, b - arm, b)},
        {at(box, a + arm, b), at(box, a, b), at(box, a, b - arm)},
    };
    for (const auto& c : corners) {
        QPainterPath path;
        path.moveTo(c[0]);
        path.lineTo(c[1]);
        path.lineTo(c[2]);
        p.drawPath(path);
    }
}

void drawDownloadIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    p.setPen(iconPen(color, box.width() * 0.10));
    p.setBrush(Qt::NoBrush);
    p.drawLine(at(box, 0.50, 0.14), at(box, 0.50, 0.62));
    QPainterPath head;
    head.moveTo(at(box, 0.28, 0.42));
    head.lineTo(at(box, 0.50, 0.64));
    head.lineTo(at(box, 0.72, 0.42));
    p.drawPath(head);
    p.drawLine(at(box, 0.20, 0.84), at(box, 0.80, 0.84));
}

void drawOpenIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    p.setPen(iconPen(color, box.width() * 0.09));
    p.setBrush(Qt::NoBrush);
    QPainterPath frame;
    frame.moveTo(at(box, 0.44, 0.22));
    frame.lineTo(at(box, 0.20, 0.22));
    frame.lineTo(at(box, 0.20, 0.80));
    frame.lineTo(at(box, 0.78, 0.80));
    frame.lineTo(at(box, 0.78, 0.56));
    p.drawPath(frame);
    p.drawLine(at(box, 0.46, 0.54), at(box, 0.82, 0.18));
    QPainterPath head;
    head.moveTo(at(box, 0.58, 0.18));
    head.lineTo(at(box, 0.82, 0.18));
    head.lineTo(at(box, 0.82, 0.42));
    p.drawPath(head);
}

void drawAlertIcon(QPainter& p, const QRectF& box, const QColor& background, const QColor& mark)
{
    p.setPen(Qt::NoPen);
    p.setBrush(background);
    p.drawEllipse(box);
    const qreal w = box.width() * 0.13;
    p.setBrush(mark);
    p.drawRoundedRect(QRectF(box.center().x() - w / 2, box.top() + box.height() * 0.22, w, box.height() * 0.36), w / 2, w / 2);
    p.drawEllipse(QPointF(box.center().x(), box.top() + box.height() * 0.74), w * 0.62, w * 0.62);
}

void drawImageGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const QRectF frame(at(box, 0.12, 0.20), at(box, 0.88, 0.80));
    p.setPen(iconPen(color, box.width() * 0.07));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(frame, box.width() * 0.08, box.width() * 0.08);
    QPainterPath hills;
    hills.moveTo(at(box, 0.20, 0.72));
    hills.lineTo(at(box, 0.40, 0.48));
    hills.lineTo(at(box, 0.54, 0.62));
    hills.lineTo(at(box, 0.64, 0.52));
    hills.lineTo(at(box, 0.80, 0.72));
    hills.closeSubpath();
    p.setPen(iconPen(color, box.width() * 0.04));
    p.setBrush(color);
    p.drawPath(hills);
    p.setPen(Qt::NoPen);
    p.drawEllipse(at(box, 0.66, 0.36), box.width() * 0.06, box.width() * 0.06);
}

// Determinate (progress >= 0) or spinning indeterminate ring.
void drawRing(QPainter& p, const QPointF& center, qreal radius, qreal width, double progress, const QColor& track, const QColor& arc)
{
    const QRectF r(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, width));
    p.drawEllipse(r);
    p.setPen(QPen(arc, width, Qt::SolidLine, Qt::RoundCap));
    if (progress < 0) {
        const int phase = static_cast<int>(QDateTime::currentMSecsSinceEpoch() % 1100);
        const int start = 90 * 16 - phase * 360 * 16 / 1100;
        p.drawArc(r, start, -110 * 16);
    } else {
        const int span = qMax(8, qRound(qBound(0.0, progress, 1.0) * 360.0));
        p.drawArc(r, 90 * 16, -span * 16);
    }
}

// ---- texts --------------------------------------------------------------------------------------

QString percentText(double progress)
{
    return i18n::t("%1%").arg(qRound(qBound(0.0, progress, 1.0) * 100.0));
}

QString joinDot(const QString& a, const QString& b)
{
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    return a + QStringLiteral("  ·  ") + b;
}

QString sizeText(const MediaEntry& e)
{
    return e.link.size ? formatSize(e.link.size) : QString();
}

// File names keep their own reading direction (from their first strong character): a name in a
// right-to-left script reads right-to-left, "2024-05-01_12-00-00.mkv" left-to-right.
// shown: the elided displayFileName(); align still decides the side (use Qt::AlignAbsolute).
void drawFileName(QPainter& p, const QRectF& rect, Qt::Alignment align, const QString& shown)
{
    const int direction = shown.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
    p.drawText(rect, static_cast<int>(align) | direction, shown);
}

bool isRetryable(const MediaEntry& e)
{
    return e.error != MediaError::NotFound && e.error != MediaError::Password;
}

// The short form; e.errorText holds the full explanation (downloadErrorText) for tooltips.
QString errorTitle(const MediaEntry& e)
{
    return downloadErrorTitle(e.error);
}

QString retryHint()
{
    return i18n::t("click to retry");
}

QString cardStatus(const MediaEntry& e, bool imageDecodeFailed)
{
    const QString size = sizeText(e);
    switch (e.state) {
    case MediaState::Idle:
        if (e.tooLargeForAuto)
            return joinDot(size, i18n::t("Large file — click to load"));
        return joinDot(size, i18n::t("Click to download"));
    case MediaState::Queued:
        return joinDot(size, i18n::t("Waiting…"));
    case MediaState::Downloading: {
        const int percent = qRound(qBound(0.0, e.progress, 1.0) * 100.0);
        if (size.isEmpty())
            return i18n::t("Downloading…  %1%").arg(percent);
        return i18n::t("Downloading…  %1% of %2").arg(percent).arg(size);
    }
    case MediaState::Ready:
        if (imageDecodeFailed)
            return joinDot(size, i18n::t("Can't preview — click to open"));
        return joinDot(size, i18n::t("Click to open"));
    case MediaState::Failed:
        return isRetryable(e) ? joinDot(errorTitle(e), retryHint()) : errorTitle(e);
    }
    return {};
}

// ---- shared overlay pieces ----------------------------------------------------------------------

// Small dark label on top of media (duration, size, GIF badge, hints).
enum class PillIcon { None, Download, Open, Progress };

QRectF drawPill(QPainter& p, const QRectF& bounds, Qt::Corner corner, PillIcon icon, double progress, const QString& text, const QFont& font)
{
    const QFontMetricsF fm(font);
    const qreal         height   = qCeil(fm.height()) + 6.0;
    const qreal         iconSize = icon == PillIcon::None ? 0.0 : height - 8.0;
    const qreal         gap      = icon == PillIcon::None || text.isEmpty() ? 0.0 : 5.0;
    const qreal         maxWidth = qMax(0.0, bounds.width() - 16.0);
    qreal               textW    = fm.horizontalAdvance(text);
    qreal               width    = 7.0 + iconSize + gap + textW + 7.0;
    if (width > maxWidth) {
        textW = qMax(0.0, maxWidth - 14.0 - iconSize - gap);
        width = maxWidth;
    }
    if (width < height)
        return {};

    const bool   right = corner == Qt::TopRightCorner || corner == Qt::BottomRightCorner;
    const bool   top   = corner == Qt::TopLeftCorner || corner == Qt::TopRightCorner;
    const qreal  x     = right ? bounds.right() - 8.0 - width : bounds.left() + 8.0;
    const qreal  y     = top ? bounds.top() + 8.0 : bounds.bottom() - 8.0 - height;
    const QRectF pill(x, y, width, height);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 165));
    p.drawRoundedRect(pill, 4, 4);

    // Icon first, then the text.
    const qreal  contentLeft = pill.left() + 7.0;
    const QRectF iconRect(contentLeft, pill.top() + 4.0, iconSize, iconSize);
    const QRectF textRect(contentLeft + iconSize + gap, pill.top(), textW, height);

    switch (icon) {
    case PillIcon::Download:
        drawDownloadIcon(p, iconRect, Qt::white);
        break;
    case PillIcon::Open:
        drawOpenIcon(p, iconRect, Qt::white);
        break;
    case PillIcon::Progress:
        drawRing(p, iconRect.center(), iconSize / 2.0 - 1.0, 2.0, progress, QColor(255, 255, 255, 70), Qt::white);
        break;
    case PillIcon::None:
        break;
    }

    if (!text.isEmpty()) {
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, fm.elidedText(text, Qt::ElideRight, textRect.width()));
    }
    return pill;
}

void drawGifBadge(QPainter& p, const QRectF& bounds, const PreviewStyle& style)
{
    const QFont         font = fontFor(style, -2, true);
    const QFontMetricsF fm(font);
    const QString       text = QStringLiteral("GIF");
    const QRectF        badge(bounds.left() + 8, bounds.top() + 8, fm.horizontalAdvance(text) + 12, qCeil(fm.height()) + 4);
    if (badge.right() > bounds.right() - 4 || badge.bottom() > bounds.bottom() - 4)
        return;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 165));
    p.drawRoundedRect(badge, 4, 4);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(badge, Qt::AlignCenter, text);
}

// Round translucent button in the middle of media (play, download, ...).
QRectF centerButton(const QRectF& bounds, qreal diameter)
{
    const qreal d = qMin(diameter, qMin(bounds.width(), bounds.height()) - 12.0);
    return centered(QSizeF(qMax(d, 20.0), qMax(d, 20.0)), bounds);
}

void drawButtonDisc(QPainter& p, const QRectF& disc, bool hovered)
{
    p.setPen(QPen(QColor(255, 255, 255, hovered ? 70 : 38), 1));
    p.setBrush(QColor(0, 0, 0, hovered ? 185 : 140));
    p.drawEllipse(disc);
}

void drawProgressDisc(QPainter& p, const QRectF& bounds, qreal diameter, double progress, bool withLabel, const PreviewStyle& style)
{
    const QRectF disc = centerButton(bounds, diameter);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawEllipse(disc);
    const qreal stroke = disc.width() >= 48 ? 3.0 : 2.5;
    drawRing(p, disc.center(), disc.width() / 2.0 - stroke - 3.0, stroke, progress, QColor(255, 255, 255, 60), Qt::white);
    if (withLabel && progress >= 0) {
        p.setFont(fontFor(style, -2, true));
        p.setPen(Qt::white);
        p.drawText(disc, Qt::AlignCenter, percentText(progress));
    }
}

// Dims the media and shows a message (errors, "can't preview") in the middle.
void drawCenterMessage(QPainter& p, const QRectF& bounds, const QString& title, const QString& hint, bool error, const PreviewStyle& style)
{
    p.fillRect(bounds, QColor(0, 0, 0, error ? 120 : 90));

    const QFont         titleFont = fontFor(style, -1, true);
    const QFont         hintFont  = fontFor(style, -2);
    const QFontMetricsF tfm(titleFont);
    const QFontMetricsF hfm(hintFont);
    const bool          withIcon  = bounds.height() >= 96;
    const bool          withHint  = !hint.isEmpty() && bounds.height() >= 64;
    const qreal         iconSize  = 26.0;
    const qreal         textWidth = qMax(0.0, bounds.width() - 24.0);
    const qreal         blockH    = (withIcon ? iconSize + 8.0 : 0.0) + tfm.height() + (withHint ? 2.0 + hfm.height() : 0.0);
    qreal               y         = bounds.center().y() - blockH / 2.0;

    if (withIcon) {
        const QRectF icon(bounds.center().x() - iconSize / 2.0, y, iconSize, iconSize);
        if (error)
            drawAlertIcon(p, icon, QColor(0xf2, 0x3f, 0x43), Qt::white);
        else
            drawImageGlyph(p, icon, QColor(255, 255, 255, 220));
        y += iconSize + 8.0;
    }
    p.setFont(titleFont);
    p.setPen(Qt::white);
    p.drawText(QRectF(bounds.left() + 12, y, textWidth, tfm.height()), Qt::AlignCenter, tfm.elidedText(title, Qt::ElideRight, textWidth));
    y += tfm.height() + 2.0;
    if (withHint) {
        p.setFont(hintFont);
        p.setPen(QColor(255, 255, 255, 190));
        p.drawText(QRectF(bounds.left() + 12, y, textWidth, hfm.height()), Qt::AlignCenter, hfm.elidedText(hint, Qt::ElideRight, textWidth));
    }
}

void drawHairline(QPainter& p, const QRectF& bounds, const QColor& color)
{
    p.setClipping(false);
    p.setPen(QPen(color, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), kRadius - 0.5, kRadius - 0.5);
}

// Blurred, dimmed copy of the picture behind it when it does not fill the preview (letterbox).
void drawBackdrop(QPainter& p, const QImage& pixels, const QRectF& bounds, bool dark)
{
    const QImage tiny = pixels.scaled(10, 10, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (tiny.isNull())
        return;
    const QImage soft = tiny.scaled(tiny.size() * 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    p.drawImage(coverRect(QSizeF(soft.size()), bounds.adjusted(-12, -12, 12, 12)), soft);
    p.fillRect(bounds, QColor(0, 0, 0, dark ? 120 : 80));
}

// ---- pictures (images, GIF stills and frames) ---------------------------------------------------

QImage renderPicture(const MediaEntry& e, const QImage& pixels, MediaStill::Source source, bool animatedFrame, const MediaLayout& layout, const PreviewStyle& style, QSize* logicalSize)
{
    const Palette pal = paletteFor(style.dark);
    if (logicalSize)
        *logicalSize = layout.box;

    QImage   out = makeCanvas(layout.box, ratioOf(style));
    QPainter p(&out);
    preparePainter(p);
    p.setLayoutDirection(Qt::LeftToRight);

    const QRectF bounds(QPointF(0, 0), QSizeF(layout.box));
    QPainterPath clip;
    clip.addRoundedRect(bounds, kRadius, kRadius);
    p.setClipPath(clip);
    p.fillRect(bounds, pal.placeholder);

    if (!pixels.isNull()) {
        const QRectF target = fitRect(QSizeF(pixels.size()), centered(layout.content, bounds));
        if (target.width() < bounds.width() - 0.75 || target.height() < bounds.height() - 0.75)
            drawBackdrop(p, pixels, bounds, style.dark);
        p.drawImage(target, pixels);
    }

    const bool animated = e.kind == MediaKind::AnimatedImage || animatedFrame;
    if (animatedFrame) {
        drawGifBadge(p, bounds, style);
        drawHairline(p, bounds, pal.hairline);
        return out;
    }

    const bool  full     = source == MediaStill::Full && !pixels.isNull();
    const QFont pillFont = fontFor(style, -2);
    switch (e.state) {
    case MediaState::Ready:
        if (full)
            break;
        if (pixels.isNull())
            drawCenterMessage(p, bounds, i18n::t("Can't preview this image"), i18n::t("click to open"), false, style);
        else
            drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Open, -1, i18n::t("Click to open"), pillFont);
        break;
    case MediaState::Queued:
        drawProgressDisc(p, bounds, 44, -1, false, style);
        break;
    case MediaState::Downloading:
        drawProgressDisc(p, bounds, 44, e.progress, false, style);
        break;
    case MediaState::Idle:
        if (source != MediaStill::Preview) {
            const QRectF disc = centerButton(bounds, 48);
            drawButtonDisc(p, disc, false);
            drawDownloadIcon(p, disc.adjusted(disc.width() * 0.27, disc.height() * 0.27, -disc.width() * 0.27, -disc.height() * 0.27), Qt::white);
        }
        drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Download, -1, sizeText(e), pillFont);
        break;
    case MediaState::Failed:
        drawCenterMessage(p, bounds, errorTitle(e), isRetryable(e) ? retryHint() : QString(), true, style);
        break;
    }
    if (animated && e.state != MediaState::Failed)
        drawGifBadge(p, bounds, style);

    drawHairline(p, bounds, pal.hairline);
    return out;
}

// ---- attachment cards (other files, previews that cannot be shown) ------------------------------

QString kindLabel(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Image:
        return QStringLiteral("IMG");
    case MediaKind::AnimatedImage:
        return QStringLiteral("GIF");
    case MediaKind::Video:
        return QStringLiteral("VID");
    case MediaKind::Audio:
        return QStringLiteral("AUD");
    case MediaKind::Archive:
        return QStringLiteral("ZIP");
    case MediaKind::Document:
        return QStringLiteral("DOC");
    case MediaKind::Other:
        break;
    }
    return QStringLiteral("FILE");
}

QColor glyphColor(const MediaEntry& e)
{
    const QString ext = QFileInfo(e.link.fileName).suffix().toLower();
    switch (e.kind) {
    case MediaKind::Image:
    case MediaKind::AnimatedImage:
        return QColor(0x58, 0x65, 0xf2);
    case MediaKind::Video:
        return QColor(0xd8, 0x3c, 0x8f);
    case MediaKind::Audio:
        return QColor(0xe0, 0x78, 0x1f);
    case MediaKind::Archive:
        return QColor(0xb9, 0x83, 0x1a);
    case MediaKind::Document:
        if (ext == QLatin1String("pdf"))
            return QColor(0xe0, 0x3e, 0x3e);
        if (ext.startsWith(QLatin1String("doc")))
            return QColor(0x2b, 0x6c, 0xd0);
        if (ext.startsWith(QLatin1String("xls")))
            return QColor(0x1e, 0x8e, 0x55);
        if (ext.startsWith(QLatin1String("ppt")))
            return QColor(0xc8, 0x4b, 0x26);
        return QColor(0x23, 0x9a, 0x55);
    case MediaKind::Other:
        break;
    }
    return QColor(0x6d, 0x77, 0x85);
}

QString extensionLabel(const MediaEntry& e)
{
    const QString ext = QFileInfo(e.link.fileName).suffix().toUpper();
    if (ext.isEmpty() || ext.size() > 4)
        return kindLabel(e.kind);
    return ext;
}

void drawFileGlyph(QPainter& p, const QRectF& r, const QColor& color, const QString& label, const PreviewStyle& style)
{
    const qreal  fold = r.width() * 0.34;
    const qreal  rad  = 4.0;
    QPainterPath body;
    body.moveTo(r.left() + rad, r.top());
    body.lineTo(r.right() - fold, r.top());
    body.lineTo(r.right(), r.top() + fold);
    body.lineTo(r.right(), r.bottom() - rad);
    body.quadTo(r.right(), r.bottom(), r.right() - rad, r.bottom());
    body.lineTo(r.left() + rad, r.bottom());
    body.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - rad);
    body.lineTo(r.left(), r.top() + rad);
    body.quadTo(r.left(), r.top(), r.left() + rad, r.top());
    body.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(body);

    QPainterPath ear;
    ear.moveTo(r.right() - fold, r.top());
    ear.lineTo(r.right() - fold, r.top() + fold - 2.5);
    ear.quadTo(r.right() - fold, r.top() + fold, r.right() - fold + 2.5, r.top() + fold);
    ear.lineTo(r.right(), r.top() + fold);
    ear.closeSubpath();
    p.setBrush(QColor(255, 255, 255, 105));
    p.drawPath(ear);

    QFont font = fontFor(style, -3.5, true);
    font.setPixelSize(label.size() >= 4 ? 8 : 9);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.2);
    p.setFont(font);
    p.setPen(Qt::white);
    const QFontMetricsF fm(font);
    const QRectF        labelRect(r.left(), r.top() + r.height() * 0.48, r.width(), r.height() * 0.42);
    p.drawText(labelRect, Qt::AlignCenter, fm.elidedText(label, Qt::ElideRight, r.width() - 2));
}

QImage renderCard(const MediaEntry& e, const PreviewStyle& style, bool imageDecodeFailed, QSize* logicalSize)
{
    const Palette pal  = paletteFor(style.dark);
    const QSize   size = cardSize(style);
    const qreal   w    = size.width();
    const qreal   h    = size.height();
    if (logicalSize)
        *logicalSize = size;

    QImage   out = makeCanvas(size, ratioOf(style));
    QPainter p(&out);
    preparePainter(p);
    p.setLayoutDirection(Qt::LeftToRight);

    p.setPen(QPen(pal.border, 1));
    p.setBrush(pal.background);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), kRadius, kRadius);

    const bool   failed = e.state == MediaState::Failed;
    const QRectF glyph(14, 11, 32, 40);
    drawFileGlyph(p, glyph, glyphColor(e), extensionLabel(e), style);
    if (failed) {
        const QRectF badge(glyph.right() - 6, glyph.bottom() - 11, 15, 15);
        p.setPen(QPen(pal.background, 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(badge.adjusted(-1, -1, 1, 1));
        drawAlertIcon(p, badge, pal.error, Qt::white);
    }

    // Action hint on the far side: download / progress / open / retry.
    enum class Action { None, Download, Progress, Open, Retry };
    Action action = Action::None;
    switch (e.state) {
    case MediaState::Idle:
        action = Action::Download;
        break;
    case MediaState::Queued:
    case MediaState::Downloading:
        action = Action::Progress;
        break;
    case MediaState::Ready:
        action = Action::Open;
        break;
    case MediaState::Failed:
        action = isRetryable(e) ? Action::Retry : Action::None;
        break;
    }
    const QRectF actionRect(w - 14 - 22, (h - 22) / 2.0, 22, 22);
    switch (action) {
    case Action::Download:
        drawDownloadIcon(p, actionRect, pal.muted);
        break;
    case Action::Progress:
        drawRing(p, actionRect.center(), 8.5, 2.2, e.state == MediaState::Queued ? -1.0 : e.progress, pal.track, kAccent);
        break;
    case Action::Open:
        drawOpenIcon(p, actionRect, pal.muted);
        break;
    case Action::Retry:
        drawCircularArrow(p, actionRect, pal.muted, 2.0);
        break;
    case Action::None:
        break;
    }

    const qreal textStart = 14 + 32 + 12;
    const qreal textEnd   = action == Action::None ? w - 14 : w - 14 - 22 - 10;
    const qreal textWidth = qMax(0.0, textEnd - textStart);

    const QFont         titleFont = fontFor(style, 0, true);
    const QFont         subFont   = fontFor(style, -1.5);
    const QFontMetricsF titleFm(titleFont);
    const QFontMetricsF subFm(subFont);
    const qreal         blockHeight = titleFm.height() + 3 + subFm.height();
    const qreal         top         = (h - blockHeight) / 2.0 - (e.state == MediaState::Downloading ? 2.0 : 0.0);
    const auto          align       = Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute;

    p.setFont(titleFont);
    p.setPen(e.state == MediaState::Ready ? pal.link : pal.title);
    drawFileName(p, QRectF(textStart, top, textWidth, titleFm.height()), align,
                 titleFm.elidedText(displayFileName(e.link.fileName), Qt::ElideMiddle, textWidth));

    p.setFont(subFont);
    p.setPen(failed ? pal.error : pal.muted);
    p.drawText(QRectF(textStart, top + titleFm.height() + 3, textWidth, subFm.height()), align,
               subFm.elidedText(cardStatus(e, imageDecodeFailed), Qt::ElideRight, textWidth));

    if (e.state == MediaState::Downloading) {
        const QRectF track(textStart, h - 9, textWidth, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.track);
        p.drawRoundedRect(track, 1.5, 1.5);
        const qreal  filled = qMax(3.0, track.width() * qBound(0.0, e.progress, 1.0));
        const QRectF done(track.topLeft(), QSizeF(filled, track.height()));
        p.setBrush(kAccent);
        p.drawRoundedRect(done, 1.5, 1.5);
    }
    return out;
}

// ---- video --------------------------------------------------------------------------------------

struct VideoGeometry {
    QRectF seekTrack; // centre line of the seek bar (3 px high)
    QRectF seekHit;
    QRectF playPause;
    QRectF mute;
    QRectF expand;
    QRectF time;
};

// Shared by renderVideo() and the hit tests so they can never disagree.
VideoGeometry videoGeometry(const QSizeF& size)
{
    const qreal   w       = size.width();
    const qreal   h       = size.height();
    const qreal   buttonY = h - 33;
    const qreal   seekY   = h - 41;
    VideoGeometry g;
    g.playPause = QRectF(6, buttonY, 28, 28);
    g.expand    = QRectF(w - 34, buttonY, 28, 28);
    g.mute      = QRectF(w - 64, buttonY, 28, 28);
    g.time      = QRectF(40, buttonY, qMax(0.0, g.mute.left() - 46), 28);
    g.seekTrack = QRectF(12, seekY - 1.5, qMax(0.0, w - 24), 3);
    g.seekHit   = QRectF(4, seekY - 7, qMax(0.0, w - 8), 13);
    return g;
}

PlaybackOverlay staticOverlay(const MediaEntry& e)
{
    PlaybackOverlay o;
    o.durationMs = e.link.durationMs;
    return o;
}

void drawVideoControls(QPainter& p, const QRectF& bounds, const PlaybackOverlay& o, qint64 durationMs, const PreviewStyle& style)
{
    const VideoGeometry g = videoGeometry(bounds.size());

    QLinearGradient shade(0, bounds.bottom() - 84, 0, bounds.bottom());
    shade.setColorAt(0.0, QColor(0, 0, 0, 0));
    shade.setColorAt(0.55, QColor(0, 0, 0, 120));
    shade.setColorAt(1.0, QColor(0, 0, 0, 200));
    p.fillRect(QRectF(bounds.left(), bounds.bottom() - 84, bounds.width(), 84), shade);

    // Seek bar: thin, grows on hover, knob only while hovered.
    const bool   seekHover = o.hover == VideoZone::Seek;
    const qreal  thickness = seekHover ? 5.0 : 3.0;
    const QRectF track(g.seekTrack.left(), g.seekTrack.center().y() - thickness / 2.0, g.seekTrack.width(), thickness);
    const double fraction  = durationMs > 0 ? qBound(0.0, static_cast<double>(o.positionMs) / static_cast<double>(durationMs), 1.0) : 0.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 72));
    p.drawRoundedRect(track, thickness / 2.0, thickness / 2.0);
    if (fraction > 0) {
        p.setBrush(kAccent);
        p.drawRoundedRect(QRectF(track.topLeft(), QSizeF(qMax(thickness, track.width() * fraction), thickness)), thickness / 2.0, thickness / 2.0);
    }
    if (seekHover) {
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(track.left() + track.width() * fraction, track.center().y()), 6.0, 6.0);
    }

    auto button = [&p, &o](const QRectF& r, VideoZone zone) {
        if (o.hover == zone) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 40));
            p.drawEllipse(r);
        }
        return r.adjusted(6, 6, -6, -6);
    };

    const QRectF playIcon = button(g.playPause, VideoZone::PlayPause);
    if (o.ended)
        drawCircularArrow(p, playIcon.adjusted(-2, -2, 2, 2), Qt::white, 2.0);
    else if (o.playing)
        drawPauseIcon(p, playIcon, Qt::white);
    else
        drawPlayIcon(p, playIcon, Qt::white);
    drawSpeakerIcon(p, button(g.mute, VideoZone::Mute), Qt::white, o.muted);
    drawExpandIcon(p, button(g.expand, VideoZone::Expand), Qt::white);

    const QFont         font = fontFor(style, -1.5);
    const QFontMetricsF fm(font);
    QString             time = formatDuration(qMax<qint64>(0, o.positionMs));
    if (durationMs > 0)
        time += QStringLiteral(" / ") + formatDuration(durationMs);
    if (fm.horizontalAdvance(time) <= g.time.width()) {
        p.setFont(font);
        p.setPen(QColor(255, 255, 255, 235));
        p.drawText(g.time, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, time);
    }
}

} // namespace

QSize previewLogicalSize(const MediaEntry& entry, const PreviewStyle& style)
{
    const QSizeF natural = linkSize(entry);
    if (entry.kind == MediaKind::Video)
        return mediaLayout(natural.isEmpty() ? QSizeF(16, 9) : natural, style, true).box;
    if (isPreviewableImage(entry.kind) && !natural.isEmpty())
        return mediaLayout(natural, style, false).box;
    return cardSize(style);
}

QImage renderPreview(const MediaEntry& entry, const MediaStill& still, const PreviewStyle& style, QSize* logicalSize)
{
    if (entry.kind == MediaKind::Video)
        return renderVideo(entry, QImage(), still, staticOverlay(entry), style, logicalSize);

    if (isPreviewableImage(entry.kind)) {
        const QSizeF natural  = linkSize(entry);
        const bool   hasStill = still.source != MediaStill::None && !still.image.isNull();
        if (!natural.isEmpty())
            return renderPicture(entry, hasStill ? still.image : QImage(), hasStill ? still.source : MediaStill::None, false, mediaLayout(natural, style, false), style, logicalSize);
        if (hasStill) {
            // Plain TeamSpeak link (no dimensions): lay out by the decoded picture.
            const QSizeF fromStill = QSizeF(still.image.size()) / ratioOf(style);
            return renderPicture(entry, still.image, still.source, false, mediaLayout(fromStill, style, false), style, logicalSize);
        }
        return renderCard(entry, style, entry.state == MediaState::Ready, logicalSize);
    }
    return renderCard(entry, style, false, logicalSize);
}

QImage renderAnimatedFrame(const MediaEntry& entry, const QImage& frame, const PreviewStyle& style, QSize* logicalSize)
{
    QSizeF natural = linkSize(entry);
    if (natural.isEmpty())
        natural = QSizeF(frame.size()) / ratioOf(style);
    return renderPicture(entry, frame, MediaStill::Full, true, mediaLayout(natural, style, false), style, logicalSize);
}

QImage renderVideo(const MediaEntry& entry, const QImage& frame, const MediaStill& poster, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize)
{
    const QSizeF natural = linkSize(entry);
    const QSize  box     = mediaLayout(natural.isEmpty() ? QSizeF(16, 9) : natural, style, true).box;
    if (logicalSize)
        *logicalSize = box;

    QImage   out = makeCanvas(box, ratioOf(style));
    QPainter p(&out);
    preparePainter(p);
    p.setLayoutDirection(Qt::LeftToRight);

    const QRectF bounds(QPointF(0, 0), QSizeF(box));
    QPainterPath clip;
    clip.addRoundedRect(bounds, kRadius, kRadius);
    p.setClipPath(clip);
    p.fillRect(bounds, Qt::black);

    const QImage& picture  = !frame.isNull() ? frame : poster.image;
    const bool    hasFrame = !frame.isNull();
    if (!picture.isNull()) {
        p.drawImage(fitRect(QSizeF(picture.size()), bounds), picture);
    } else {
        QLinearGradient bg(0, 0, 0, bounds.height());
        bg.setColorAt(0, QColor(0x26, 0x27, 0x2c));
        bg.setColorAt(1, QColor(0x10, 0x11, 0x13));
        p.fillRect(bounds, bg);
    }

    const qint64 durationMs = overlay.durationMs > 0 ? overlay.durationMs : entry.link.durationMs;
    const bool   started    = hasFrame || overlay.playing || overlay.ended || overlay.positionMs > 0;
    const QFont  pillFont   = fontFor(style, -2);

    // No poster: show the file name so the box is not anonymous.
    if (picture.isNull() && !started) {
        QLinearGradient top(0, 0, 0, 40);
        top.setColorAt(0, QColor(0, 0, 0, 120));
        top.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(0, 0, bounds.width(), 40), top);
        const QFont         font = fontFor(style, -1, true);
        const QFontMetricsF fm(font);
        p.setFont(font);
        p.setPen(QColor(255, 255, 255, 225));
        const QRectF nameRect(12, 8, bounds.width() - 24, fm.height());
        drawFileName(p, nameRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute,
                     fm.elidedText(displayFileName(entry.link.fileName), Qt::ElideMiddle, nameRect.width()));
    }

    const bool failed = entry.state == MediaState::Failed && !hasFrame && !overlay.busy;
    if (failed) {
        drawCenterMessage(p, bounds, errorTitle(entry), isRetryable(entry) ? retryHint() : QString(), true, style);
    } else if (overlay.busy) {
        drawProgressDisc(p, bounds, kPlayButton, overlay.busyProgress, true, style);
    } else if (!overlay.playing) {
        const QRectF disc    = centerButton(bounds, kPlayButton);
        const bool   hovered = overlay.hover == VideoZone::Body || overlay.hover == VideoZone::PlayPause;
        drawButtonDisc(p, disc, hovered);
        const QRectF icon = disc.adjusted(disc.width() * 0.28, disc.height() * 0.28, -disc.width() * 0.28, -disc.height() * 0.28);
        if (overlay.ended)
            drawCircularArrow(p, icon.adjusted(-2, -2, 2, 2), Qt::white, 2.6);
        else
            drawPlayIcon(p, icon.translated(disc.width() * 0.02, 0), Qt::white);
    }

    if (!failed) {
        if (overlay.controlsVisible && started) {
            drawVideoControls(p, bounds, overlay, durationMs, style);
        } else if (overlay.playing) {
            // Controls hidden while playing: a hairline of progress and a muted hint.
            if (durationMs > 0) {
                const double fraction = qBound(0.0, static_cast<double>(overlay.positionMs) / static_cast<double>(durationMs), 1.0);
                p.fillRect(QRectF(0, bounds.height() - 3, bounds.width(), 3), QColor(255, 255, 255, 50));
                p.fillRect(QRectF(0, bounds.height() - 3, bounds.width() * fraction, 3), kAccent);
            }
            if (overlay.muted) {
                const QRectF disc(bounds.width() - 34, bounds.height() - 36, 26, 26);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 150));
                p.drawEllipse(disc);
                drawSpeakerIcon(p, disc.adjusted(6, 6, -6, -6), Qt::white, true);
            }
        } else {
            if (durationMs > 0)
                drawPill(p, bounds, Qt::BottomRightCorner, PillIcon::None, -1, formatDuration(durationMs), pillFont);
            if (!overlay.busy && !started) {
                switch (entry.state) {
                case MediaState::Idle:
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Download, -1, sizeText(entry), pillFont);
                    break;
                case MediaState::Queued:
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, -1, i18n::t("Waiting…"), pillFont);
                    break;
                case MediaState::Downloading:
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, entry.progress, percentText(entry.progress), pillFont);
                    break;
                case MediaState::Ready:
                case MediaState::Failed:
                    break;
                }
            }
        }
    }

    drawHairline(p, bounds, QColor(255, 255, 255, style.dark ? 18 : 0));
    return out;
}

VideoZone videoZoneAt(const QSize& logicalSize, const QPointF& pos)
{
    const QRectF bounds(QPointF(0, 0), QSizeF(logicalSize));
    if (logicalSize.isEmpty() || !bounds.contains(pos))
        return VideoZone::None;
    const VideoGeometry g = videoGeometry(bounds.size());
    if (g.playPause.contains(pos))
        return VideoZone::PlayPause;
    if (g.mute.contains(pos))
        return VideoZone::Mute;
    if (g.expand.contains(pos))
        return VideoZone::Expand;
    if (g.seekHit.contains(pos))
        return VideoZone::Seek;
    return VideoZone::Body;
}

double seekFractionAt(const QSize& logicalSize, const QPointF& pos)
{
    const VideoGeometry g = videoGeometry(QSizeF(logicalSize));
    if (g.seekTrack.width() <= 0)
        return 0.0;
    return qBound(0.0, (pos.x() - g.seekTrack.left()) / g.seekTrack.width(), 1.0);
}
