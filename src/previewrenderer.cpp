#include "previewrenderer.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "previewpaint.h" // 2.2 audio
#include "uiutil.h"

namespace {

constexpr qreal kRadius            = 8.0;
constexpr int   kCardWidth         = 340;
constexpr int   kCardHeight        = 64;
constexpr qreal kCardPadding       = 16.0; // left and right
constexpr qreal kCardSlot          = 24.0; // action slot on the right
constexpr qreal kCardGap           = 8.0;  // between the text and the action slot
constexpr int   kRetryLabelMin     = 300;  // narrower cards show the retry arrow without its label
constexpr qreal kMinImageWidth     = 120.0;
constexpr qreal kMinImageHeight    = 40.0;
constexpr qreal kMinVideoWidth     = 240.0;
constexpr qreal kMinVideoHeight    = 135.0;
constexpr qreal kMaxUpscale        = 2.0; // small pictures may grow this much to make a usable click target
constexpr qreal kPlayButton        = 56.0;
constexpr qreal kSmallPlayButton   = 44.0; // players under kSmallPlayerHeight: clears the seek bar
constexpr qreal kSmallPlayerHeight = 180.0;
constexpr qreal kMediaButton       = 48.0; // download / progress / waiting disc on pictures

// Overlays on top of media. Measured against the worst case (white pixels behind them), see
// previewColorPairs(): pills 6.9:1, message plate title 8.8:1 and hint 7.0:1, control text 6.5:1.
constexpr int   kPillAlpha        = 165;
constexpr int   kDimAlpha         = 60; // whole preview behind a message: context, not legibility
constexpr int   kErrorDimAlpha    = 90;
constexpr int   kPlateAlpha       = 158; // local plate behind the message text
constexpr int   kPlateHoverAlpha  = 185;
constexpr int   kPlatePressAlpha  = 210;
constexpr int   kHintAlpha        = 217;
constexpr qreal kShadeHeight      = 96.0; // video controls: gradient from this far above the bottom
constexpr int   kShadeSeekAlpha   = 158;  // ... at the seek bar
constexpr int   kShadeEndAlpha    = 215;  // ... at the bottom edge
constexpr int   kTimeAlpha        = 235;
constexpr qreal kSeekFromBottom   = 41.0;
constexpr qreal kButtonFromBottom = 33.0;

const QColor kAccent(0x58, 0x65, 0xf2);
const QColor kAlertRed(0xf2, 0x3f, 0x43); // alert disc on media (non-text: 3:1 is enough)

struct Palette {
    QColor background, backgroundHover, border, title, link, muted;
    QColor errorText; // error messages (text: at least 4.5:1 on background and backgroundHover)
    QColor errorFill; // the alert disc (non-text)
    QColor track, progress; // progress ring / bar: progress at least 3:1 against track
    QColor placeholder, placeholderHover, hairline;
};

Palette paletteFor(bool dark)
{
    if (dark)
        return {QColor(0x2b, 0x2d, 0x31), QColor(0x30, 0x32, 0x36), QColor(0x1e, 0x1f, 0x22), QColor(0xf2, 0xf3, 0xf5), QColor(0x00, 0xa8, 0xfc),
                QColor(0x94, 0x9b, 0xa4), QColor(0xfa, 0x77, 0x7a), QColor(0xf2, 0x3f, 0x43), QColor(0x3a, 0x3c, 0x42), QColor(0x79, 0x83, 0xf5),
                QColor(0x23, 0x24, 0x28), QColor(0x2a, 0x2c, 0x30), QColor(255, 255, 255, 22)};
    return {QColor(0xf2, 0xf3, 0xf5), QColor(0xeb, 0xed, 0xef), QColor(0xdc, 0xde, 0xe2), QColor(0x06, 0x06, 0x07), QColor(0x00, 0x5f, 0xcc),
            QColor(0x5c, 0x5e, 0x66), QColor(0xc4, 0x28, 0x2d), QColor(0xda, 0x37, 0x3c), QColor(0xd4, 0xd7, 0xdc), QColor(0x58, 0x65, 0xf2),
            QColor(0xe3, 0xe5, 0xe8), QColor(0xdc, 0xde, 0xe2), QColor(0, 0, 0, 26)};
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

// The first text that fits width (callers list them from most to least informative); the last one
// elided if none does.
QString fitText(const QFontMetricsF& fm, const QStringList& texts, qreal width)
{
    for (const QString& text : texts) {
        if (fm.horizontalAdvance(text) <= width)
            return text;
    }
    return texts.isEmpty() ? QString() : fm.elidedText(texts.last(), Qt::ElideRight, width);
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

// One weight for every outline icon: about a ninth of its box, in quarter pixels (1.75 px for the
// 16 px control icons, 2.5 px for the 22 px card icons). Play and pause are filled instead.
qreal strokeFor(const QRectF& box)
{
    return qMax(1.5, qRound(box.width() / 9.0 * 4.0) / 4.0);
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

// Circular arrow (replay / retry): open circle with an open chevron at the top pointing left (↺),
// drawn with the same pen as the arc.
void drawCircularArrow(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal  stroke = strokeFor(box);
    const QRectF ring   = box.adjusted(box.width() * 0.19, box.height() * 0.21, -box.width() * 0.19, -box.height() * 0.17);
    QPainterPath path;
    path.arcMoveTo(ring, 90.0);
    path.arcTo(ring, 90.0, -280.0);
    const QPointF tip(ring.center().x() - stroke * 0.25, ring.top());
    const qreal   arm = box.width() * 0.26 * 0.7071; // 45 degrees off the arc's start tangent
    path.moveTo(tip + QPointF(arm, -arm));
    path.lineTo(tip);
    path.lineTo(tip + QPointF(arm, arm));
    p.setPen(iconPen(color, stroke));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
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
    p.setPen(iconPen(color, strokeFor(box)));
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
    p.setPen(iconPen(color, strokeFor(box)));
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
    p.setPen(iconPen(color, strokeFor(box)));
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
    p.setPen(iconPen(color, strokeFor(box)));
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

// A folder with its tab: "show in folder", for files that are never opened from the chat.
void drawFolderIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    p.setPen(iconPen(color, strokeFor(box)));
    p.setBrush(Qt::NoBrush);
    QPainterPath folder;
    folder.moveTo(at(box, 0.14, 0.78));
    folder.lineTo(at(box, 0.14, 0.24));
    folder.lineTo(at(box, 0.40, 0.24));
    folder.lineTo(at(box, 0.50, 0.34));
    folder.lineTo(at(box, 0.86, 0.34));
    folder.lineTo(at(box, 0.86, 0.78));
    folder.closeSubpath();
    p.drawPath(folder);
    p.drawLine(at(box, 0.14, 0.46), at(box, 0.86, 0.46));
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

// Three dots in a row (waiting), centred on center.
void drawWaitingDots(QPainter& p, const QPointF& center, qreal diameter, qreal spacing, const QColor& color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (int i = -1; i <= 1; ++i)
        p.drawEllipse(QPointF(center.x() + i * spacing, center.y()), diameter / 2.0, diameter / 2.0);
}

// Waiting in the queue (or busy while Windows animations are off): the full track ring with three
// dots inside. Static and unlike any progress arc, so it can't be read as "stuck at 30%".
void drawWaitingGlyph(QPainter& p, const QPointF& center, qreal radius, qreal width, const QColor& track, const QColor& dots)
{
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, width));
    p.drawEllipse(center, radius, radius);
    const qreal dot = qMax(2.0, radius * 0.2);
    drawWaitingDots(p, center, dot, qMax(dot * 1.9, radius * 0.5), dots);
}

// Determinate (progress >= 0) or indeterminate ring: spinning, or the static waiting glyph when
// animations are off.
void drawRing(QPainter& p, const QPointF& center, qreal radius, qreal width, double progress, const QColor& track, const QColor& arc, bool animate)
{
    if (progress < 0 && !animate) {
        drawWaitingGlyph(p, center, radius, width, track, arc);
        return;
    }
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

// Scales what is drawn while it lives to 96% around center (a pressed button), when on.
class PressScale
{
  public:
    PressScale(QPainter& p, const QPointF& center, bool on)
        : m_p(p)
        , m_on(on)
    {
        if (!m_on)
            return;
        m_p.save();
        m_p.translate(center);
        m_p.scale(0.96, 0.96);
        m_p.translate(-center);
    }
    ~PressScale()
    {
        if (m_on)
            m_p.restore();
    }
    PressScale(const PressScale&)            = delete;
    PressScale& operator=(const PressScale&) = delete;

  private:
    QPainter& m_p;
    bool      m_on;
};

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
    return a + QStringLiteral(" · ") + b;
}

QString sizeText(const MediaEntry& e)
{
    return e.link.size ? formatSize(e.link.size) : QString();
}

// "4.5 MB of 10.0 MB" for a download in progress, empty if the size is unknown.
QString amountText(const MediaEntry& e)
{
    if (!e.link.size)
        return {};
    const auto done = static_cast<quint64>(qBound(0.0, e.progress, 1.0) * static_cast<double>(e.link.size) + 0.5);
    return formatProgress(done, e.link.size);
}

// File names keep their own reading direction (from their first strong character): a name in a
// right-to-left script reads right-to-left, "2024-05-01_12-00-00.mkv" left-to-right.
// shown: the elided displayNameFor(); align still decides the side (use Qt::AlignAbsolute).
void drawFileName(QPainter& p, const QRectF& rect, Qt::Alignment align, const QString& shown)
{
    const int direction = shown.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
    p.drawText(rect, static_cast<int>(align) | direction, shown);
}

// The short form; e.errorText holds the full explanation (downloadErrorText) for tooltips.
QString errorTitle(const MediaEntry& e)
{
    return downloadErrorTitle(e.error);
}

QString retryHint()
{
    return i18n::t("Click to retry");
}

// 2.2 sha: files above this show the check's percentage (and their ring follows the check).
constexpr quint64 kCheckPercentFromBytes = 256ull * 1024 * 1024;

bool checkingPercent(const MediaEntry& e)
{
    return isCheckingShown(e) && e.link.size > kCheckPercentFromBytes && e.check.progress >= 0;
}

// A preview's status from most to least complete, without the file size: the card shows the first
// one that fits (after "size · first"), the tooltip the first.
QStringList statusTexts(const MediaEntry& e, bool cannotPreview, bool revealOnly)
{
    switch (e.state) {
    case MediaState::Idle:
        if (e.heldByDataSaver && !e.tooLargeForAuto) // 2.2 data saver
            return {i18n::t("Data saver — click to load"), i18n::t("Click to load"), i18n::t("Download")};
        return {i18n::t("Click to download"), i18n::t("Download")};
    case MediaState::Queued:
        return {i18n::t("Waiting to download…"), i18n::t("Waiting…")};
    case MediaState::Downloading: {
        if (isCheckingShown(e)) // 2.2 sha
            return {checkingText(e), i18n::t("Checking…")};
        const int     percent = qRound(qBound(0.0, e.progress, 1.0) * 100.0);
        const QString amounts = amountText(e);
        QStringList   texts;
        if (!amounts.isEmpty())
            texts << i18n::t("Downloading… %1% · %2").arg(percent).arg(amounts);
        texts << i18n::t("Downloading… %1%").arg(percent) << percentText(e.progress);
        return texts;
    }
    case MediaState::Ready:
        if (cannotPreview)
            return {i18n::t("Can't preview · Click to open"), i18n::t("Can't preview")};
        if (revealOnly)
            return {i18n::t("Click to show in folder"), i18n::t("Show in folder")};
        return {i18n::t("Click to open"), i18n::t("Open")};
    case MediaState::Failed:
        if (e.error == MediaError::Mismatch) { // 2.2 sha: the cause and the way out, as much as fits
            const QString again = i18n::t("Ask the sender again");
            return {joinDot(errorTitle(e), errorHint(e)), joinDot(errorTitle(e), again), joinDot(i18n::t("Doesn't match"), again), errorTitle(e),
                    i18n::t("Doesn't match")};
        }
        return {errorTitle(e)};
    }
    return {};
}

// The card's status line candidates: the size goes first, and is the first thing dropped.
QStringList cardStatusTexts(const MediaEntry& e, bool cannotPreview, bool revealOnly)
{
    QStringList   texts = statusTexts(e, cannotPreview, revealOnly);
    const QString size  = sizeText(e);
    const bool    sized = e.state != MediaState::Downloading && e.state != MediaState::Failed;
    if (sized && !size.isEmpty() && !texts.isEmpty())
        texts.prepend(joinDot(size, texts.first()));
    return texts;
}

// ---- shared overlay pieces ----------------------------------------------------------------------

// Small dark label on top of media (duration, size, GIF badge, hints).
enum class PillIcon { None, Download, Open, Progress };

// texts: from most to least complete, the first that fits is shown. maxWidth < 0: the preview's
// width minus the insets (pass less when another pill shares the edge). Returns the pill's rect.
QRectF drawPill(QPainter& p, const QRectF& bounds, Qt::Corner corner, PillIcon icon, double progress, const QStringList& texts, const QFont& font,
                qreal maxWidth = -1.0)
{
    const QFontMetricsF fm(font);
    const qreal         height   = qCeil(fm.height()) + 6.0;
    const qreal         iconSize = icon == PillIcon::None ? 0.0 : height - 8.0;
    const bool          hasText  = !texts.isEmpty() && !texts.first().isEmpty();
    const qreal         gap      = icon == PillIcon::None || !hasText ? 0.0 : 5.0;
    const qreal         limit    = qMin(maxWidth < 0 ? bounds.width() - 16.0 : maxWidth, bounds.width() - 16.0);
    const QString       text     = hasText ? fitText(fm, texts, qMax(0.0, limit - 14.0 - iconSize - gap)) : QString();
    const qreal         textW    = qMin(fm.horizontalAdvance(text), qMax(0.0, limit - 14.0 - iconSize - gap));
    const qreal         width    = 7.0 + iconSize + gap + textW + 7.0;
    if (width < height || width > limit + 0.5)
        return {};

    const bool   right = corner == Qt::TopRightCorner || corner == Qt::BottomRightCorner;
    const bool   top   = corner == Qt::TopLeftCorner || corner == Qt::TopRightCorner;
    const qreal  x     = right ? bounds.right() - 8.0 - width : bounds.left() + 8.0;
    const qreal  y     = top ? bounds.top() + 8.0 : bounds.bottom() - 8.0 - height;
    const QRectF pill(x, y, width, height);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, kPillAlpha));
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
        if (progress < 0) // waiting: too small for the ring with dots, the dots alone
            drawWaitingDots(p, iconRect.center(), qMax(2.0, iconSize * 0.18), iconSize * 0.34, Qt::white);
        else
            drawRing(p, iconRect.center(), iconSize / 2.0 - 1.0, 2.0, progress, QColor(255, 255, 255, 70), Qt::white, true);
        break;
    case PillIcon::None:
        break;
    }

    if (!text.isEmpty()) {
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, text);
    }
    return pill;
}

void drawGifBadge(QPainter& p, const QRectF& bounds, const PreviewStyle& style)
{
    drawPill(p, bounds, Qt::TopLeftCorner, PillIcon::None, -1, {QStringLiteral("GIF")}, fontFor(style, -2, true));
}

// Round translucent button in the middle of media (play, download, ...).
QRectF centerButton(const QRectF& bounds, qreal diameter)
{
    const qreal d = qMin(diameter, qMin(bounds.width(), bounds.height()) - 12.0);
    return centered(QSizeF(qMax(d, 20.0), qMax(d, 20.0)), bounds);
}

void drawButtonDisc(QPainter& p, const QRectF& disc, bool hovered, bool pressed)
{
    p.setPen(QPen(QColor(255, 255, 255, hovered || pressed ? 70 : 38), 1));
    p.setBrush(QColor(0, 0, 0, pressed ? 210 : hovered ? 185 : 140));
    p.drawEllipse(disc);
}

QRectF discIcon(const QRectF& disc, qreal inset)
{
    return disc.adjusted(disc.width() * inset, disc.height() * inset, -disc.width() * inset, -disc.height() * inset);
}

// Download progress (or waiting) in a disc. progress < 0: spinner, or the waiting glyph when
// waiting is set or animations are off.
void drawProgressDisc(QPainter& p, const QRectF& disc, double progress, bool waiting, bool withLabel, const PreviewStyle& style, bool hovered, bool pressed)
{
    PressScale scale(p, disc.center(), pressed);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, pressed ? 210 : hovered ? 185 : 150));
    p.drawEllipse(disc);
    const qreal stroke = disc.width() >= 48 ? 3.0 : 2.5;
    const qreal radius = disc.width() / 2.0 - stroke - 3.0;
    if (waiting && progress < 0)
        drawWaitingGlyph(p, disc.center(), radius, stroke, QColor(255, 255, 255, 60), Qt::white);
    else
        drawRing(p, disc.center(), radius, stroke, progress, QColor(255, 255, 255, 60), Qt::white, style.animate);
    if (withLabel && progress >= 0) {
        p.setFont(fontFor(style, -2, true));
        p.setPen(Qt::white);
        p.drawText(disc, Qt::AlignCenter, percentText(progress));
    }
}

// A message (errors, "can't preview") in the middle of a preview. Over media (overMedia): the
// picture is dimmed a little and the text sits on a local dark plate, so it stays readable over
// any pixels. Without media behind it: the theme's placeholder with theme colours.
void drawCenterMessage(QPainter& p, const QRectF& bounds, const QString& title, const QString& hint, bool error, bool overMedia, const PreviewStyle& style,
                       bool hovered, bool pressed)
{
    const Palette       pal       = paletteFor(style.dark);
    const QFont         titleFont = fontFor(style, 0, true);
    const QFont         hintFont  = fontFor(style, -1);
    const QFontMetricsF tfm(titleFont);
    const QFontMetricsF hfm(hintFont);
    const bool          withIcon  = bounds.height() >= 96;
    const bool          withHint  = !hint.isEmpty() && bounds.height() >= 64;
    const qreal         iconSize  = 26.0;
    const qreal         padX      = 12.0;
    const qreal         padY      = 8.0;
    const qreal         textMax   = qMax(0.0, bounds.width() - 16.0 - 2.0 * padX);
    const QString       shownTitle = tfm.elidedText(title, Qt::ElideRight, textMax);
    const QString       shownHint  = withHint ? hfm.elidedText(hint, Qt::ElideRight, textMax) : QString();
    const qreal         blockH     = (withIcon ? iconSize + 8.0 : 0.0) + tfm.height() + (withHint ? 2.0 + hfm.height() : 0.0);

    QColor titleColor = pal.title;
    QColor hintColor  = pal.muted;
    QColor glyphColor = pal.muted;
    QColor alertColor = pal.errorFill;
    if (overMedia) {
        p.fillRect(bounds, QColor(0, 0, 0, error ? kErrorDimAlpha : kDimAlpha));
        qreal contentW = tfm.horizontalAdvance(shownTitle);
        if (withHint)
            contentW = qMax(contentW, hfm.horizontalAdvance(shownHint));
        if (withIcon)
            contentW = qMax(contentW, iconSize);
        const QRectF plate = centered(QSizeF(qMin(contentW + 2.0 * padX, bounds.width() - 16.0), qMin(blockH + 2.0 * padY, bounds.height() - 8.0)), bounds);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, pressed ? kPlatePressAlpha : hovered ? kPlateHoverAlpha : kPlateAlpha));
        p.drawRoundedRect(plate, 8, 8);
        titleColor = Qt::white;
        hintColor  = QColor(255, 255, 255, kHintAlpha);
        glyphColor = QColor(255, 255, 255, 220);
        alertColor = kAlertRed;
    } else if (hovered || pressed) {
        p.fillRect(bounds, pal.placeholderHover);
    }

    qreal y = bounds.center().y() - blockH / 2.0;
    if (withIcon) {
        const QRectF icon(bounds.center().x() - iconSize / 2.0, y, iconSize, iconSize);
        if (error)
            drawAlertIcon(p, icon, alertColor, Qt::white);
        else
            drawImageGlyph(p, icon, glyphColor);
        y += iconSize + 8.0;
    }
    const qreal textLeft = bounds.center().x() - textMax / 2.0;
    p.setFont(titleFont);
    p.setPen(titleColor);
    p.drawText(QRectF(textLeft, y, textMax, tfm.height()), Qt::AlignCenter, shownTitle);
    y += tfm.height() + 2.0;
    if (withHint) {
        p.setFont(hintFont);
        p.setPen(hintColor);
        p.drawText(QRectF(textLeft, y, textMax, hfm.height()), Qt::AlignCenter, shownHint);
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

    // Pointer feedback only where a click does something (not on files gone from the server).
    const bool   hovered  = style.hovered && isPreviewActionable(e);
    const bool   pressed  = hovered && style.pressed;
    const bool   full     = source == MediaStill::Full && !pixels.isNull();
    const QFont  pillFont = fontFor(style, -2);
    const QRectF disc     = centerButton(bounds, kMediaButton);
    switch (e.state) {
    case MediaState::Ready:
        if (full)
            break;
        if (pixels.isNull())
            drawCenterMessage(p, bounds, i18n::t("Can't preview this image"), i18n::t("Click to open"), false, false, style, hovered, pressed);
        else
            drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Open, -1, {i18n::t("Click to open")}, pillFont);
        break;
    case MediaState::Queued:
        drawProgressDisc(p, disc, -1, true, false, style, hovered, pressed);
        drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::None, -1, {i18n::t("Waiting to download…"), i18n::t("Waiting…")}, pillFont);
        break;
    case MediaState::Downloading: {
        if (isCheckingShown(e)) { // 2.2 sha: downloaded, being checked
            const double shown = shownDownloadProgress(e);
            drawProgressDisc(p, disc, shown, false, checkingPercent(e), style, hovered, pressed);
            drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, shown, {checkingText(e), i18n::t("Checking…")}, pillFont);
            break;
        }
        // Same disc as the download button before, now with the percentage; the pill has the amounts.
        drawProgressDisc(p, disc, e.progress, false, true, style, hovered, pressed);
        const QString amounts = amountText(e);
        QStringList   texts;
        if (!amounts.isEmpty())
            texts << joinDot(percentText(e.progress), amounts);
        texts << percentText(e.progress);
        drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, e.progress, texts, pillFont);
        break;
    }
    case MediaState::Idle: {
        if (source != MediaStill::Preview) {
            PressScale scale(p, disc.center(), pressed);
            drawButtonDisc(p, disc, hovered, pressed);
            drawDownloadIcon(p, discIcon(disc, 0.27), Qt::white);
        }
        drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Download, -1, {sizeText(e)}, pillFont);
        break;
    }
    case MediaState::Failed:
        drawCenterMessage(p, bounds, errorTitle(e), errorHint(e), true, !pixels.isNull(), style, hovered, pressed);
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

// Every fill keeps at least 4.5:1 with the white extension label on it.
QColor glyphColor(const MediaEntry& e)
{
    const QString ext = QFileInfo(e.link.fileName).suffix().toLower();
    switch (e.kind) {
    case MediaKind::Image:
    case MediaKind::AnimatedImage:
        return QColor(0x58, 0x65, 0xf2);
    case MediaKind::Video:
        return QColor(0xd5, 0x2d, 0x86);
    case MediaKind::Audio:
        return QColor(0xb1, 0x5f, 0x18);
    case MediaKind::Archive:
        return QColor(0x97, 0x6b, 0x15);
    case MediaKind::Document:
        if (ext == QLatin1String("pdf"))
            return QColor(0xde, 0x2f, 0x2f);
        if (ext.startsWith(QLatin1String("doc")))
            return QColor(0x2b, 0x6c, 0xd0);
        if (ext.startsWith(QLatin1String("xls")))
            return QColor(0x1c, 0x83, 0x4f);
        if (ext.startsWith(QLatin1String("ppt")))
            return QColor(0xc8, 0x4b, 0x26);
        return QColor(0x1e, 0x85, 0x4a);
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

    // 9 px ("ZIP") / 8 px ("DOCX") at the default chat font, growing with larger chat fonts.
    QFont font = fontFor(style, -3.5, true);
    font.setPixelSize(qBound(9, qRound(basePixelSize(style.font)) - 3, 11) - (label.size() >= 4 ? 1 : 0));
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

    // Hover: a slightly lighter (dark theme) or darker (light theme) card, the action in the title
    // colour; pressed: the action in the accent. Cards that do nothing on click get neither.
    const bool   failed     = e.state == MediaState::Failed;
    const bool   hovered    = style.hovered && isPreviewActionable(e);
    const bool   pressed    = hovered && style.pressed;
    const QColor background = hovered ? pal.backgroundHover : pal.background;
    const QColor actionInk  = pressed ? pal.progress : hovered ? pal.title : pal.muted;
    p.setPen(QPen(pal.border, 1));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), kRadius, kRadius);

    const QRectF glyph(kCardPadding, (h - 40.0) / 2.0, 32, 40);
    drawFileGlyph(p, glyph, glyphColor(e), extensionLabel(e), style);
    if (failed) {
        const QRectF badge(glyph.right() - 6, glyph.bottom() - 11, 15, 15);
        p.setPen(QPen(background, 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(badge.adjusted(-1, -1, 1, 1));
        drawAlertIcon(p, badge, pal.errorFill, Qt::white);
    }

    // Action on the far side: download / waiting / progress / open / retry (labelled when there is
    // room). Files that can't be fetched again (deleted, password) get no action at all.
    const QFont         actionFont = fontFor(style, -1);
    const QFontMetricsF actionFm(actionFont);
    const bool          retry      = failed && isRetryableDownload(e);
    const bool          retryLabel = retry && w >= kRetryLabelMin;
    const qreal         slotWidth  = retryLabel ? 20.0 + 4.0 + actionFm.horizontalAdvance(i18n::t("Retry")) : kCardSlot;
    const QRectF        slot(w - kCardPadding - slotWidth, (h - kCardSlot) / 2.0, slotWidth, kCardSlot);
    const QRectF        icon = slot.adjusted(1, 1, -1, -1);
    bool                hasAction = true;
    switch (e.state) {
    case MediaState::Idle:
        drawDownloadIcon(p, icon, actionInk);
        break;
    case MediaState::Queued:
        drawWaitingGlyph(p, slot.center(), 8.5, 2.2, pal.track, hovered ? pal.title : pal.muted);
        break;
    case MediaState::Downloading:
        drawRing(p, slot.center(), 8.5, 2.2, shownDownloadProgress(e), pal.track, pal.progress, style.animate); // 2.2 sha: the check's, when shown
        break;
    case MediaState::Ready:
        // Programs and scripts are only shown in their folder (the status line says so).
        if (style.revealOnly)
            drawFolderIcon(p, icon, actionInk);
        else
            drawOpenIcon(p, icon, actionInk);
        break;
    case MediaState::Failed:
        hasAction = retry;
        if (retryLabel) {
            drawCircularArrow(p, QRectF(slot.left(), slot.center().y() - 10.0, 20, 20), actionInk);
            p.setFont(actionFont);
            p.setPen(hovered ? pal.title : pal.muted); // text: the accent would fall below 4.5:1
            p.drawText(QRectF(slot.left() + 24.0, slot.top(), slotWidth - 24.0, slot.height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute,
                       i18n::t("Retry"));
        } else if (retry) {
            drawCircularArrow(p, icon, actionInk);
        }
        break;
    }

    const qreal textStart = glyph.right() + 12.0;
    const qreal textEnd   = hasAction ? slot.left() - kCardGap : w - kCardPadding;
    const qreal textWidth = qMax(0.0, textEnd - textStart);

    // The text block sits at the same place in every state: the progress bar has its own row at the
    // bottom, which large chat fonts keep clear of.
    const QFont         titleFont = fontFor(style, 0, true);
    const QFont         subFont   = fontFor(style, -1.5);
    const QFontMetricsF titleFm(titleFont);
    const QFontMetricsF subFm(subFont);
    const qreal         blockHeight = titleFm.height() + 3 + subFm.height();
    const qreal         top         = qMax(2.0, qMin((h - blockHeight) / 2.0, h - 14.0 - blockHeight));
    const auto          align       = Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute;

    p.setFont(titleFont);
    p.setPen(e.state == MediaState::Ready ? pal.link : pal.title);
    drawFileName(p, QRectF(textStart, top, textWidth, titleFm.height()), align, titleFm.elidedText(displayNameFor(e.link), Qt::ElideMiddle, textWidth));

    // The status drops the size first, then uses a short form; it is only cut off as a last resort.
    p.setFont(subFont);
    p.setPen(failed ? pal.errorText : pal.muted);
    p.drawText(QRectF(textStart, top + titleFm.height() + 3, textWidth, subFm.height()), align,
               fitText(subFm, cardStatusTexts(e, imageDecodeFailed, style.revealOnly), textWidth));

    if (e.state == MediaState::Downloading) {
        const QRectF track(textStart, h - 10, textWidth, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.track);
        p.drawRoundedRect(track, 1.5, 1.5);
        const qreal  filled = qMax(3.0, track.width() * qBound(0.0, shownDownloadProgress(e), 1.0)); // 2.2 sha
        const QRectF done(track.topLeft(), QSizeF(filled, track.height()));
        p.setBrush(pal.progress);
        p.drawRoundedRect(done, 1.5, 1.5);
    }
    return out;
}

// ---- video --------------------------------------------------------------------------------------

struct VideoGeometry {
    QRectF centerDisc; // play / replay / progress button in the middle
    QRectF seekTrack;  // centre line of the seek bar (3 px high)
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
    const qreal   buttonY = h - kButtonFromBottom;
    const qreal   seekY   = h - kSeekFromBottom;
    VideoGeometry g;
    // Small players get a smaller centre button, so it stays clear of the seek bar.
    g.centerDisc = centerButton(QRectF(QPointF(0, 0), size), h < kSmallPlayerHeight ? kSmallPlayButton : kPlayButton);
    g.playPause  = QRectF(6, buttonY, 28, 28);
    g.expand     = QRectF(w - 34, buttonY, 28, 28);
    g.mute       = QRectF(w - 64, buttonY, 28, 28);
    g.time       = QRectF(40, buttonY, qMax(0.0, g.mute.left() - 46), 28);
    g.seekTrack  = QRectF(12, seekY - 1.5, qMax(0.0, w - 24), 3);
    g.seekHit    = QRectF(4, seekY - 10, qMax(0.0, w - 8), 18); // ends where the buttons start
    return g;
}

bool inCenterDisc(const VideoGeometry& g, const QPointF& pos)
{
    const QPointF d = pos - g.centerDisc.center();
    return std::hypot(d.x(), d.y()) <= g.centerDisc.width() / 2.0;
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

    // Dark enough at the seek bar and the buttons for white over any frame (white pixels included).
    const qreal     shadeTop = bounds.bottom() - kShadeHeight;
    QLinearGradient shade(0, shadeTop, 0, bounds.bottom());
    shade.setColorAt(0.0, QColor(0, 0, 0, 0));
    shade.setColorAt((kShadeHeight - kSeekFromBottom) / kShadeHeight, QColor(0, 0, 0, kShadeSeekAlpha));
    shade.setColorAt(1.0, QColor(0, 0, 0, kShadeEndAlpha));
    p.fillRect(QRectF(bounds.left(), shadeTop, bounds.width(), kShadeHeight), shade);

    // Seek bar: thin, grows on hover. The playhead is always a white dot, so the position does not
    // depend on telling the accent from the track.
    const bool   seekHover = o.hover == VideoZone::Seek || o.pressed == VideoZone::Seek;
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
    p.setBrush(Qt::white);
    const qreal knob = seekHover ? 6.0 : 4.0;
    p.drawEllipse(QPointF(track.left() + track.width() * fraction, track.center().y()), knob, knob);

    auto button = [&p, &o](const QRectF& r, VideoZone zone) {
        if (o.pressed == zone || o.hover == zone) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, o.pressed == zone ? 70 : 40));
            p.drawEllipse(r);
        }
        return r.adjusted(6, 6, -6, -6);
    };

    const QRectF playIcon = button(g.playPause, VideoZone::PlayPause);
    if (o.ended)
        drawCircularArrow(p, playIcon.adjusted(-2, -2, 2, 2), Qt::white);
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
        p.setPen(QColor(255, 255, 255, kTimeAlpha));
        p.drawText(g.time, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, time);
    }
}

} // namespace

// ---- 2.2 audio: the helpers above for preview modules in other files (previewpaint.h) -----------
namespace previewpaint {

Palette palette(bool dark)
{
    const ::Palette p = paletteFor(dark);
    return {p.background, p.backgroundHover, p.border, p.title, p.link, p.muted, p.errorText, p.errorFill,
            p.track, p.progress, p.placeholder, p.placeholderHover, p.hairline};
}
QColor  accent() { return kAccent; }
qreal   cornerRadius() { return kRadius; }
QFont   font(const PreviewStyle& style, qreal delta, bool bold) { return fontFor(style, delta, bold); }
qreal   ratio(const PreviewStyle& style) { return ratioOf(style); }
QImage  canvas(const QSize& logical, qreal dpr) { return makeCanvas(logical, dpr); }
void    prepare(QPainter& p) { preparePainter(p); }
QString fitText(const QFontMetricsF& fm, const QStringList& texts, qreal width) { return ::fitText(fm, texts, width); }
void    drawFileName(QPainter& p, const QRectF& rect, Qt::Alignment align, const QString& shown) { ::drawFileName(p, rect, align, shown); }
void    drawPlayIcon(QPainter& p, const QRectF& box, const QColor& color) { ::drawPlayIcon(p, box, color); }
void    drawPauseIcon(QPainter& p, const QRectF& box, const QColor& color) { ::drawPauseIcon(p, box, color); }
void    drawCircularArrow(QPainter& p, const QRectF& box, const QColor& color) { ::drawCircularArrow(p, box, color); }
void    drawOpenIcon(QPainter& p, const QRectF& box, const QColor& color) { ::drawOpenIcon(p, box, color); }
void    drawDownloadIcon(QPainter& p, const QRectF& box, const QColor& color) { ::drawDownloadIcon(p, box, color); }
void    drawAlertIcon(QPainter& p, const QRectF& box, const QColor& background, const QColor& mark) { ::drawAlertIcon(p, box, background, mark); }
void    drawRing(QPainter& p, const QPointF& center, qreal radius, qreal width, double progress, const QColor& track, const QColor& arc, bool animate)
{
    ::drawRing(p, center, radius, width, progress, track, arc, animate);
}

} // namespace previewpaint
// ---- end 2.2 audio --------------------------------------------------------------------------------

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

    const VideoGeometry g          = videoGeometry(bounds.size());
    const qint64        durationMs = overlay.durationMs > 0 ? overlay.durationMs : entry.link.durationMs;
    const bool          started    = hasFrame || overlay.playing || overlay.ended || overlay.positionMs > 0;
    const QFont         pillFont   = fontFor(style, -2);
    const bool          hovered    = overlay.hover == VideoZone::Body || overlay.hover == VideoZone::PlayPause;
    const bool          pressed    = overlay.pressed == VideoZone::Body || overlay.pressed == VideoZone::PlayPause;

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
                     fm.elidedText(displayNameFor(entry.link), Qt::ElideMiddle, nameRect.width()));
    }

    const bool failed = entry.state == MediaState::Failed && !hasFrame && !overlay.busy;
    if (failed) {
        const bool retry = isRetryableDownload(entry);
        drawCenterMessage(p, bounds, errorTitle(entry), errorHint(entry), true, true, style, retry && overlay.hover != VideoZone::None,
                          retry && overlay.pressed != VideoZone::None);
    } else if (overlay.busy) {
        // 2.2 sha: while a smaller file is checked the ring stays full, without a "100%" that would
        // contradict "Checking file…".
        const bool label = !isCheckingShown(entry) || checkingPercent(entry);
        drawProgressDisc(p, g.centerDisc, overlay.busyProgress, false, label, style, overlay.hover != VideoZone::None, overlay.pressed != VideoZone::None);
    } else if (!overlay.playing) {
        const QRectF disc = g.centerDisc;
        PressScale   scale(p, disc.center(), pressed);
        drawButtonDisc(p, disc, hovered, pressed);
        if (overlay.externalOnly)
            drawOpenIcon(p, discIcon(disc, 0.3), Qt::white); // opens in the default app
        else if (overlay.ended)
            drawCircularArrow(p, discIcon(disc, 0.28).adjusted(-2, -2, 2, 2), Qt::white);
        else
            drawPlayIcon(p, discIcon(disc, 0.28).translated(disc.width() * 0.02, 0), Qt::white);
    }

    if (!failed) {
        if (overlay.controlsVisible && started) {
            p.setOpacity(qBound(0.0, overlay.controlsOpacity, 1.0));
            drawVideoControls(p, bounds, overlay, durationMs, style);
            p.setOpacity(1.0);
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
            QRectF right;
            if (durationMs > 0)
                right = drawPill(p, bounds, Qt::BottomRightCorner, PillIcon::None, -1, {formatDuration(durationMs)}, pillFont);
            // The left pill must leave the duration readable.
            const qreal leftMax = right.isValid() ? right.left() - 8.0 - bounds.left() - 8.0 : -1.0;
            if (overlay.externalOnly && !overlay.busy) {
                drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Open, -1, {i18n::t("Opens in default app")}, pillFont, leftMax);
            } else if (isCheckingShown(entry) && !started) { // 2.2 sha: downloaded, being checked
                drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, shownDownloadProgress(entry), {checkingText(entry), i18n::t("Checking…")}, pillFont,
                         leftMax);
            } else if (!overlay.busy && !started) {
                switch (entry.state) {
                case MediaState::Idle:
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Download, -1, {sizeText(entry)}, pillFont, leftMax);
                    break;
                case MediaState::Queued:
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, -1, {i18n::t("Waiting…")}, pillFont, leftMax);
                    break;
                case MediaState::Downloading: {
                    const QString amounts = amountText(entry);
                    QStringList   texts;
                    if (!amounts.isEmpty())
                        texts << joinDot(percentText(entry.progress), amounts);
                    texts << percentText(entry.progress);
                    drawPill(p, bounds, Qt::BottomLeftCorner, PillIcon::Progress, entry.progress, texts, pillFont, leftMax);
                    break;
                }
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

VideoZone videoZoneAt(const QSize& logicalSize, const QPointF& pos, bool centerButton)
{
    const QRectF bounds(QPointF(0, 0), QSizeF(logicalSize));
    if (logicalSize.isEmpty() || !bounds.contains(pos))
        return VideoZone::None;
    const VideoGeometry g = videoGeometry(bounds.size());
    if (centerButton && inCenterDisc(g, pos))
        return VideoZone::PlayPause;
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

QRectF videoZoneRect(const QSize& logicalSize, const QPointF& pos, bool centerButton)
{
    const QRectF bounds(QPointF(0, 0), QSizeF(logicalSize));
    const VideoGeometry g = videoGeometry(bounds.size());
    switch (videoZoneAt(logicalSize, pos, centerButton)) {
    case VideoZone::None:
        return {};
    case VideoZone::PlayPause:
        return centerButton && inCenterDisc(g, pos) ? g.centerDisc : g.playPause;
    case VideoZone::Mute:
        return g.mute;
    case VideoZone::Expand:
        return g.expand;
    case VideoZone::Seek:
        return g.seekHit;
    case VideoZone::Body:
        break;
    }
    return bounds;
}

double seekFractionAt(const QSize& logicalSize, const QPointF& pos)
{
    const VideoGeometry g = videoGeometry(QSizeF(logicalSize));
    if (g.seekTrack.width() <= 0)
        return 0.0;
    return qBound(0.0, (pos.x() - g.seekTrack.left()) / g.seekTrack.width(), 1.0);
}

bool isRetryableDownload(const MediaEntry& entry)
{
    // 2.2 sha: a file that doesn't match its link would come back with the same bytes.
    return entry.state == MediaState::Failed && entry.error != MediaError::NotFound && entry.error != MediaError::Password && entry.error != MediaError::Mismatch;
}

// ---- 2.2 sha ------------------------------------------------------------------------------------

bool isCheckingShown(const MediaEntry& entry)
{
    return entry.state == MediaState::Downloading && entry.check.running && entry.check.shown;
}

double shownDownloadProgress(const MediaEntry& entry)
{
    if (!isCheckingShown(entry))
        return entry.progress;
    return checkingPercent(entry) ? qBound(0.0, entry.check.progress, 1.0) : 1.0;
}

QString checkingText(const MediaEntry& entry)
{
    const bool percent = checkingPercent(entry);
    const int  value   = qRound(qBound(0.0, entry.check.progress, 1.0) * 100.0);
    if (entry.check.again)
        return percent ? i18n::t("Checking file again… %1%").arg(value) : i18n::t("Checking file again…");
    return percent ? i18n::t("Checking file… %1%").arg(value) : i18n::t("Checking file…");
}

QString errorHint(const MediaEntry& entry)
{
    if (entry.state != MediaState::Failed)
        return {};
    if (entry.error == MediaError::Mismatch)
        return i18n::t("Ask the sender to send it again");
    return isRetryableDownload(entry) ? retryHint() : QString();
}

bool isPreviewActionable(const MediaEntry& entry)
{
    return entry.state != MediaState::Failed || isRetryableDownload(entry);
}

QString previewStatusText(const MediaEntry& entry, bool cannotPreview, bool revealOnly)
{
    return statusTexts(entry, cannotPreview, revealOnly).value(0);
}

QVector<PreviewColorPair> previewColorPairs(bool dark)
{
    const Palette             pal = paletteFor(dark);
    QVector<PreviewColorPair> pairs;
    auto add = [&pairs](const char* name, const QColor& foreground, const QColor& background, double minimum) {
        pairs.append({QString::fromLatin1(name), ui::flatten(foreground, background), background, minimum});
    };

    // Cards, at rest and hovered.
    for (const bool hover : {false, true}) {
        const QColor bg = hover ? pal.backgroundHover : pal.background;
        add(hover ? "card title (hover)" : "card title", pal.title, bg, 4.5);
        add(hover ? "card link name (hover)" : "card link name", pal.link, bg, 4.5);
        add(hover ? "card status (hover)" : "card status", pal.muted, bg, 4.5);
        add(hover ? "card error status (hover)" : "card error status", pal.errorText, bg, 4.5);
        add(hover ? "card alert disc (hover)" : "card alert disc", pal.errorFill, bg, 3.0);
        add(hover ? "card progress vs card (hover)" : "card progress vs card", pal.progress, bg, 3.0);
    }
    add("card action icon (hover)", pal.title, pal.backgroundHover, 3.0);
    add("card action icon (pressed)", pal.progress, pal.backgroundHover, 3.0);
    add("card progress vs track", pal.progress, pal.track, 3.0);

    // Messages without media behind them ("Can't preview this image").
    add("placeholder title", pal.title, pal.placeholder, 4.5);
    add("placeholder hint", pal.muted, pal.placeholder, 4.5);
    add("placeholder hint (hover)", pal.muted, pal.placeholderHover, 4.5);
    add("placeholder alert disc", pal.errorFill, pal.placeholder, 3.0);

    // File-type badges: white labels on every glyph colour.
    for (const char* name : {"clip.mp4", "song.mp3", "files.zip", "report.pdf", "notes.docx", "sheet.xlsx", "slides.pptx", "readme.txt", "photo.png", "setup.exe"}) {
        MediaEntry e;
        e.link.fileName = QString::fromLatin1(name);
        e.kind          = kindForFileName(e.link.fileName);
        pairs.append({QStringLiteral("glyph label ") + extensionLabel(e), Qt::white, glyphColor(e), 4.5});
    }

    // On top of media, over the worst case: white pixels.
    const QColor white(Qt::white);
    const QColor pill = ui::flatten(QColor(0, 0, 0, kPillAlpha), white);
    add("pill text over white", white, pill, 4.5);
    for (const bool error : {false, true}) {
        const QColor dimmed = ui::flatten(QColor(0, 0, 0, error ? kErrorDimAlpha : kDimAlpha), white);
        const QColor plate  = ui::flatten(QColor(0, 0, 0, kPlateAlpha), dimmed);
        add(error ? "error message title over white" : "message title over white", white, plate, 4.5);
        add(error ? "error message hint over white" : "message hint over white", QColor(255, 255, 255, kHintAlpha), plate, 4.5);
    }
    // The time label's top edge is the lightest part of the shade behind it.
    auto shadeAt = [](qreal fromBottom) {
        const qreal seekStop = (kShadeHeight - kSeekFromBottom) / kShadeHeight;
        const qreal t        = (kShadeHeight - fromBottom) / kShadeHeight;
        const qreal alpha    = t <= seekStop ? kShadeSeekAlpha * t / seekStop : kShadeSeekAlpha + (kShadeEndAlpha - kShadeSeekAlpha) * (t - seekStop) / (1.0 - seekStop);
        return QColor(0, 0, 0, qRound(alpha));
    };
    const QColor timeBg = ui::flatten(shadeAt(kButtonFromBottom), white);
    add("video time over white", QColor(255, 255, 255, kTimeAlpha), timeBg, 4.5);
    add("video control icons over white", white, timeBg, 3.0);
    add("video playhead over white", white, ui::flatten(shadeAt(kSeekFromBottom), white), 3.0);
    return pairs;
}

// 2.2 drag-out
void drawFileTypeGlyph(QPainter& p, const QRectF& rect, const MediaEntry& entry, const PreviewStyle& style)
{
    drawFileGlyph(p, rect, glyphColor(entry), extensionLabel(entry), style);
}
