#include "linkpreview/linkcard.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QtMath>

#include <cmath>

#include "i18n.h"
#include "medialink.h"
#include "previewpaint.h"
#include "spoiler.h"
#include "uiutil.h"

using namespace previewpaint;

namespace linkcard {

namespace {

constexpr int   kStripWidth   = 340; // the file card's width
constexpr int   kStripHeight  = 56;
constexpr int   kRichWidth    = 400; // YouTube / Poster
constexpr int   kMinWidth     = 160; // like the file card: never wider than the chat's column (its floor is 160)
constexpr qreal kPad          = 12.0;
constexpr qreal kPadOnBubble  = 2.0;
constexpr qreal kIcon         = 32.0;
constexpr qreal kHide         = 20.0; // the hide button's room
constexpr qreal kBar          = 4.0;  // YouTube's coloured bar
constexpr qreal kGap          = 8.0;
constexpr qreal kPlayDisc     = 48.0;
constexpr qreal kPillHeight   = 26.0;
constexpr qreal kActionLabelMin = 280.0; // narrower strips show the action's icon only

bool rich(const Card& card)
{
    return card.look == Look::YouTube || card.look == Look::Poster;
}

qreal padOf(const PreviewStyle& style)
{
    return onBubble(style) ? kPadOnBubble : kPad;
}

// The kind's colour (glyph and YouTube's bar): non-text, 3:1 on the card is enough.
QColor tintOf(linkpreview::Kind kind, bool dark)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
        return dark ? QColor(0xff, 0x4e, 0x45) : QColor(0xd9, 0x1e, 0x18);
    case linkpreview::Kind::Gif:
        return dark ? QColor(0xc0, 0x84, 0xfc) : QColor(0x86, 0x3b, 0xd6);
    case linkpreview::Kind::Video:
        return dark ? QColor(0x3b, 0xa5, 0x5d) : QColor(0x24, 0x80, 0x45);
    default:
        return dark ? QColor(0x00, 0xa8, 0xfc) : QColor(0x00, 0x6c, 0xe0);
    }
}

QColor readableTint(linkpreview::Kind kind, const PreviewStyle& style, const Palette& pal)
{
    QVector<QColor> backs = style.bubbles;
    if (backs.isEmpty())
        backs.append(pal.background);
    return ui::readableOn(tintOf(kind, style.dark), backs, 3.0);
}

QString actionText(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        return i18n::t("Load preview");
    case Look::Error:
        return i18n::t("Retry");
    case Look::TooLarge:
    case Look::Poster:
        return i18n::t("Open in browser");
    default:
        return {};
    }
}

QPen roundPen(const QColor& color, qreal width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// ---- glyphs (vector paths, the stroke weight of the other card icons) -------------------------------

qreal strokeOf(const QRectF& box)
{
    return qMax(1.5, std::round(box.width() / 9.0 * 4.0) / 4.0);
}

void drawPictureGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal  s = strokeOf(box);
    const QRectF r = box.adjusted(box.width() * 0.12, box.height() * 0.2, -box.width() * 0.12, -box.height() * 0.2);
    p.setPen(roundPen(color, s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 2.5, 2.5);
    p.drawEllipse(QPointF(r.left() + r.width() * 0.3, r.top() + r.height() * 0.32), r.width() * 0.08, r.width() * 0.08);
    QPainterPath hills;
    hills.moveTo(r.left() + r.width() * 0.08, r.bottom() - r.height() * 0.12);
    hills.lineTo(r.left() + r.width() * 0.42, r.top() + r.height() * 0.5);
    hills.lineTo(r.left() + r.width() * 0.62, r.top() + r.height() * 0.72);
    hills.lineTo(r.left() + r.width() * 0.75, r.top() + r.height() * 0.58);
    hills.lineTo(r.right() - r.width() * 0.06, r.bottom() - r.height() * 0.12);
    p.drawPath(hills);
}

void drawVideoGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal  s = strokeOf(box);
    const QRectF r = box.adjusted(box.width() * 0.1, box.height() * 0.22, -box.width() * 0.1, -box.height() * 0.22);
    p.setPen(roundPen(color, s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 3, 3);
    QPainterPath tri;
    const QPointF c = r.center();
    const qreal   t = r.height() * 0.24;
    tri.moveTo(c.x() - t * 0.8, c.y() - t);
    tri.lineTo(c.x() + t * 1.1, c.y());
    tri.lineTo(c.x() - t * 0.8, c.y() + t);
    tri.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(tri);
}

void drawGifGlyph(QPainter& p, const QRectF& box, const QColor& color, const PreviewStyle& style)
{
    const qreal  s = strokeOf(box);
    const QRectF r = box.adjusted(box.width() * 0.08, box.height() * 0.24, -box.width() * 0.08, -box.height() * 0.24);
    p.setPen(roundPen(color, s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 3, 3);
    QFont f = font(style, -3, true);
    f.setPixelSize(qMax(7, qRound(r.height() * 0.62)));
    p.setFont(f);
    p.setPen(color);
    p.drawText(r, Qt::AlignCenter, QString::fromLatin1("GIF"));
}

void drawKindGlyph(QPainter& p, const QRectF& box, linkpreview::Kind kind, const QColor& color, const PreviewStyle& style)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
    case linkpreview::Kind::Video:
        drawVideoGlyph(p, box, color);
        break;
    case linkpreview::Kind::Gif:
        drawGifGlyph(p, box, color, style);
        break;
    default:
        drawPictureGlyph(p, box, color);
        break;
    }
}

void drawCloseGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const QRectF r = box.adjusted(box.width() * 0.3, box.height() * 0.3, -box.width() * 0.3, -box.height() * 0.3);
    p.setPen(roundPen(color, 1.75));
    p.drawLine(r.topLeft(), r.bottomRight());
    p.drawLine(r.topRight(), r.bottomLeft());
}

// The 16:9 picture of a YouTube card or a poster: the image covering it, or the placeholder (a ring on it
// while it loads); the play disc in the middle.
void drawThumb(QPainter& p, const Card& card, const Layout& l, const Palette& pal, const PreviewStyle& style, bool hovered, bool pressed)
{
    const qreal  radius = 6.0;
    QPainterPath clip;
    clip.addRoundedRect(l.thumb, radius, radius);
    p.save();
    p.setClipPath(clip);
    p.fillRect(l.thumb, hovered ? pal.placeholderHover : pal.placeholder);
    if (!card.thumbnail.isNull()) {
        const QSizeF img    = QSizeF(card.thumbnail.size());
        const QSizeF scaled = img.scaled(l.thumb.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(l.thumb.center().x() - scaled.width() / 2.0, l.thumb.center().y() - scaled.height() / 2.0, scaled.width(), scaled.height());
        p.drawImage(target, card.thumbnail);
    }
    p.restore();
    if (card.thumbnail.isNull() && card.thumbnailLoading) {
        drawRing(p, l.thumb.center(), 12, 2.5, -1.0, pal.track, pal.progress, style.animate);
        return;
    }
    // The play disc: dark and translucent like the video player's (white glyph, 4.5:1 on it over white).
    const QColor disc = pressed ? QColor(0, 0, 0, 215) : hovered ? QColor(0, 0, 0, 190) : QColor(0, 0, 0, 165);
    p.setPen(Qt::NoPen);
    p.setBrush(disc);
    p.drawEllipse(l.play);
    drawPlayIcon(p, l.play.adjusted(l.play.width() * 0.3, l.play.height() * 0.3, -l.play.width() * 0.26, -l.play.height() * 0.3), Qt::white);
    if (card.look == Look::Poster && card.durationMs > 0) {
        const QFont         f = font(style, -2, true);
        const QFontMetricsF fm(f);
        const QString       d = formatDuration(card.durationMs);
        const QRectF        badge(l.thumb.right() - 8 - fm.horizontalAdvance(d) - 12, l.thumb.bottom() - 8 - fm.height() - 4, fm.horizontalAdvance(d) + 12,
                                  fm.height() + 4);
        p.setPen(Qt::NoPen); // the play glyph's pen would ring the badge (the player's badge has none)
        p.setBrush(QColor(0, 0, 0, 165));
        p.drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(badge, Qt::AlignCenter, d);
    }
}

QString shownTitle(const Card& card)
{
    return card.title.isEmpty() ? kindLabel(card.kind) : card.title;
}

// A YouTube title as the card shows it: the lines it needs, at most two (the second elided). A card gets its title
// with its look (the announcement, the oEmbed answer), so its height never changes while it is shown.
QStringList titleLines(const QString& title, const QFont& font, qreal width)
{
    QTextLayout tl(title, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl.setTextOption(option);
    tl.beginLayout();
    QStringList lines;
    int         lastStart = 0;
    bool        more      = false;
    for (int i = 0; i < 3; ++i) {
        QTextLine line = tl.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (i == 2) {
            more = true;
            break;
        }
        lastStart = line.textStart();
        lines << title.mid(line.textStart(), line.textLength()).trimmed();
    }
    tl.endLayout();
    if (more && !lines.isEmpty())
        lines.last() = QFontMetricsF(font).elidedText(title.mid(lastStart).simplified() + QChar(0x2026), Qt::ElideRight, width);
    if (lines.isEmpty())
        lines << QString();
    return lines;
}

} // namespace

// ---- texts ----------------------------------------------------------------------------------------------

QString kindLabel(linkpreview::Kind kind)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
        return i18n::t("YouTube video");
    case linkpreview::Kind::Image:
        return i18n::t("Picture");
    case linkpreview::Kind::Gif:
        return i18n::t("GIF");
    case linkpreview::Kind::Video:
        return i18n::t("Video");
    case linkpreview::Kind::None:
        break;
    }
    return {};
}

QString cleanText(const QString& text, int maxChars)
{
    QString out;
    out.reserve(qMin(text.size(), maxChars + 2));
    bool space = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar  c = text.at(i);
        const ushort u = c.unicode();
        const bool   bidi = (u >= 0x200e && u <= 0x200f) || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069) || u == 0x061c;
        if (c.isSpace()) { // line breaks and tabs too
            space = !out.isEmpty();
            continue;
        }
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format || bidi || u == 0xfffc || u == 0xfffd)
            continue;
        if (space) {
            out += QLatin1Char(' ');
            space = false;
        }
        if (c.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate())
                continue;
            if (out.size() + 2 > maxChars)
                break;
            out += c;
            out += text.at(++i);
            continue;
        }
        if (c.isLowSurrogate())
            continue;
        if (out.size() + 1 > maxChars)
            break;
        out += c;
    }
    return out;
}

QString statusText(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        if (card.size > 0)
            return i18n::t("%1 · %2").arg(kindLabel(card.kind), formatSize(static_cast<quint64>(card.size)));
        if (card.overAutoLimit && card.autoLimit > 0) // the site didn't say how large: at least the limit
            return i18n::t("%1 · over %2").arg(kindLabel(card.kind), formatSize(static_cast<quint64>(card.autoLimit)));
        return kindLabel(card.kind);
    case Look::Loading:
        if (card.waiting == Waiting::Own)
            return i18n::t("Making the preview…");
        if (card.waiting == Waiting::Sender)
            return i18n::t("Waiting for the sender's preview…");
        if (card.waiting == Waiting::Checking)
            return i18n::t("Checking for the sender's preview…");
        return card.progress >= 0 ? i18n::t("Loading preview… %1%").arg(qRound(qBound(0.0, card.progress, 1.0) * 100)) : i18n::t("Loading preview…");
    case Look::Error:
        return card.error.isEmpty() ? i18n::t("Couldn't load the preview") : card.error;
    case Look::TooLarge:
        return card.size > 0 ? i18n::t("Too large to preview here · %1").arg(formatSize(static_cast<quint64>(card.size))) : i18n::t("Too large to preview here");
    case Look::YouTube:
        return card.title;
    case Look::Poster:
        return i18n::t("Too large to play here");
    case Look::Hidden:
        return i18n::t("Preview hidden");
    }
    return {};
}

QString toolTip(const Card& card, Zone zone)
{
    // Whom a click on Load preview / Retry contacts: the link's site for a file; for YouTube never the link's
    // own address (youtu.be, m.youtube.com, ...) but YouTube's (www.youtube.com, i.ytimg.com).
    const QString site = card.kind == linkpreview::Kind::YouTube ? QString::fromLatin1("YouTube") : card.host;
    switch (zone) {
    case Zone::Hide:
        return i18n::t("Hide preview");
    case Zone::Show:
        return i18n::t("Show the preview again");
    case Zone::Open:
        // A YouTube card is one Open zone (zoneAt): its tip is the card's own, with the whole title and channel the
        // card may cut short (live test: the title never showed).
        if (card.look == Look::YouTube)
            break;
        return i18n::t("Opens %1 in your browser").arg(card.host);
    case Zone::Retry:
        return i18n::t("Try loading the preview again. This connects your PC to %1, which sees your IP address.").arg(site);
    case Zone::Load:
    case Zone::Body:
        break;
    case Zone::None:
        return {};
    }
    switch (card.look) {
    case Look::Click:
        if (card.size > 0 || card.overAutoLimit)
            return i18n::t("Larger than your limit for automatic downloads. Loading the preview connects your PC to %1, which sees your IP address.").arg(site);
        return i18n::t("Loading the preview connects your PC to %1, which sees your IP address.").arg(site);
    case Look::Loading:
        if (card.waiting == Waiting::Own)
            return i18n::t("Your TS Media is loading this link once to share its preview in this chat.");
        if (card.waiting == Waiting::Sender)
            return i18n::t("The sender's TS Media is sharing a preview of this link through the TeamSpeak server. Your PC doesn't contact %1 for it.").arg(site);
        if (card.waiting == Waiting::Checking)
            return i18n::t("Checking whether the sender's TS Media shares a preview of this link. If it doesn't, the preview may be loaded from %1, which sees your IP address.").arg(site);
        return i18n::t("Loading the preview from %1…").arg(site);
    case Look::Error:
        return card.error.isEmpty() ? i18n::t("The preview couldn't be loaded from %1.").arg(site) : card.error;
    case Look::TooLarge:
        return i18n::t("This file is too large to preview in the chat. Open it in your browser instead.");
    case Look::YouTube: {
        QString tip = card.title.isEmpty() ? kindLabel(card.kind) : card.title;
        if (!card.author.isEmpty())
            tip += QLatin1Char('\n') + card.author;
        return tip + QLatin1Char('\n') + i18n::t("Opens on %1 in your browser").arg(card.host);
    }
    case Look::Poster:
        return i18n::t("This video is too large to play in the chat. Opens on %1 in your browser.").arg(card.host);
    case Look::Hidden:
        return i18n::t("Show the preview again");
    }
    return {};
}

QString toolTipHtml(const Card& card, Zone zone)
{
    const QString plain = toolTip(card, zone);
    if (plain.isEmpty())
        return {};
    QString html;
    for (const QString& line : plain.split(QLatin1Char('\n'))) {
        if (!html.isEmpty())
            html += QString::fromLatin1("<br>");
        html += line.toHtmlEscaped();
    }
    return QString::fromLatin1("<p style='white-space:pre'>") + html + QString::fromLatin1("</p>");
}

// ---- geometry -----------------------------------------------------------------------------------------------

Layout layout(const Card& card, const PreviewStyle& style)
{
    Layout      l;
    const qreal pad = padOf(style);
    if (card.look == Look::Hidden) {
        const QFont         f = font(style, -1.5);
        const QFontMetricsF fm(f);
        const qreal         w = 10 + 14 + 6 + fm.horizontalAdvance(i18n::t("Preview hidden")) + 6 + fm.horizontalAdvance(QString(QChar(0x00b7))) + 6
                        + fm.horizontalAdvance(i18n::t("Show")) + 12;
        l.size   = QSize(qCeil(qMin<qreal>(w, qMax(160, style.maxWidth))), qRound(kPillHeight));
        l.icon   = QRectF(10, (kPillHeight - 14) / 2.0, 14, 14);
        l.text   = QRectF(l.icon.right() + 6, 0, l.size.width() - l.icon.right() - 6 - 10, kPillHeight);
        l.action = QRectF(0, 0, l.size.width(), kPillHeight);
        return l;
    }
    if (!rich(card)) {
        const int w = qMin(kStripWidth, qMax(kMinWidth, style.maxWidth));
        l.size      = QSize(w, kStripHeight);
        l.icon      = QRectF(pad, (kStripHeight - kIcon) / 2.0, kIcon, kIcon);
        l.hide      = QRectF(w - (onBubble(style) ? 0 : 6) - kHide, (kStripHeight - kHide) / 2.0, kHide, kHide);
        const QString text = actionText(card);
        qreal         right = l.hide.left() - 4;
        if (!text.isEmpty()) {
            const QFontMetricsF fm(font(style, -1, true));
            const qreal         label = w >= kActionLabelMin ? fm.horizontalAdvance(text) + 6 : 0;
            const qreal         aw    = 18 + label;
            l.action                  = QRectF(right - aw, (kStripHeight - 24) / 2.0, aw, 24);
            right                     = l.action.left() - kGap;
        }
        l.text = QRectF(l.icon.right() + 10, 0, qMax(0.0, right - l.icon.right() - 10), kStripHeight);
        return l;
    }
    // YouTube / Poster
    const int   w  = qMin(kRichWidth, qMax(kMinWidth, style.maxWidth));
    const qreal x0 = card.look == Look::YouTube ? pad + kBar + 10 : pad;
    const qreal cw = w - x0 - pad;
    qreal       y  = onBubble(style) ? 4 : kPad;
    if (card.look == Look::YouTube) {
        const QFontMetricsF small(font(style, -1.5, true));
        const QFontMetricsF big(font(style, 0, true));
        l.hide  = QRectF(w - (onBubble(style) ? 0 : 6) - kHide, y - 2, kHide, kHide);
        y += small.height() + 3;                                   // the channel
        // The title's lines (one or two; it comes with the card, so the card never grows while it is shown).
        l.title = QRectF(x0, y, cw, titleLines(shownTitle(card), font(style, 0, true), cw).size() * big.lineSpacing());
        y       = l.title.bottom() + 8;
    } else {
        l.hide = QRectF(w - (onBubble(style) ? 0 : 6) - kHide - 4, y + 4, kHide, kHide); // on the poster's corner
    }
    l.thumb = QRectF(x0, y, cw, std::round(cw * 9.0 / 16.0));
    l.play  = QRectF(l.thumb.center().x() - kPlayDisc / 2.0, l.thumb.center().y() - kPlayDisc / 2.0, kPlayDisc, kPlayDisc);
    y       = l.thumb.bottom() + 6;
    const QFontMetricsF foot(font(style, -1.5));
    l.footer = QRectF(x0, y, cw, qMax(foot.height(), 20.0));
    if (card.look == Look::Poster) {
        // Its label where there is room (as on the strips); a narrow card keeps the site line readable.
        const QFontMetricsF fm(font(style, -1, true));
        const qreal         aw = w >= kActionLabelMin ? 18 + fm.horizontalAdvance(actionText(card)) + 6 : 18;
        l.action               = QRectF(l.footer.right() - aw, l.footer.top() + (l.footer.height() - 20) / 2.0, aw, 20);
    }
    y      = l.footer.bottom() + (onBubble(style) ? 4 : kPad - 2);
    l.size = QSize(w, qCeil(y));
    return l;
}

Zone primaryZone(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        return Zone::Load;
    case Look::Error:
        return Zone::Retry;
    case Look::TooLarge:
    case Look::YouTube:
    case Look::Poster:
        return Zone::Open;
    case Look::Hidden:
        return Zone::Show;
    case Look::Loading:
        return Zone::Body;
    }
    return Zone::Body;
}

bool isActionable(const Card& card, Zone zone)
{
    if (zone == Zone::Hide || zone == Zone::Show || zone == Zone::Load || zone == Zone::Retry || zone == Zone::Open)
        return true;
    return zone == Zone::Body && card.look != Look::Loading;
}

Zone zoneAt(const Card& card, const Layout& l, const QPointF& pos, bool over)
{
    if (!QRectF(QPointF(0, 0), QSizeF(l.size)).contains(pos))
        return Zone::None;
    if (card.look == Look::Hidden)
        return Zone::Show;
    if (over && l.hide.adjusted(-2, -2, 2, 2).contains(pos))
        return Zone::Hide;
    if (!l.action.isEmpty() && l.action.adjusted(-4, -6, 4, 6).contains(pos))
        return primaryZone(card);
    if (card.look == Look::Loading)
        return Zone::Body;
    return primaryZone(card); // the whole card does its one thing
}

// ---- drawing ----------------------------------------------------------------------------------------------

QImage render(const Card& card, const PreviewStyle& style, const Pointer& pointer, Layout* layoutOut)
{
    const Layout  l   = layout(card, style);
    const Palette pal = palette(style);
    if (layoutOut)
        *layoutOut = l;
    QImage   out = canvas(l.size, ratio(style));
    QPainter p(&out);
    prepare(p);
    p.setLayoutDirection(Qt::LeftToRight);
    const qreal w        = l.size.width();
    const qreal h        = l.size.height();
    const bool  hovered  = pointer.over && isActionable(card, pointer.hover == Zone::None ? Zone::Body : pointer.hover);
    const bool  pressed  = hovered && pointer.pressed != Zone::None && pointer.pressed == pointer.hover;
    const QColor tint    = readableTint(card.kind, style, pal);
    const auto  align    = Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute;

    // ---- Hidden: the pill ----
    if (card.look == Look::Hidden) {
        p.setPen(onBubble(style) ? Qt::NoPen : QPen(pal.border, 1));
        p.setBrush(hovered ? pal.placeholderHover : pal.placeholder);
        p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), (h - 1) / 2.0, (h - 1) / 2.0);
        spoiler::drawEyeOffIcon(p, l.icon, pal.muted);
        const QFont         f = font(style, -1.5);
        const QFontMetricsF fm(f);
        p.setFont(f);
        p.setPen(pal.muted);
        const QString hidden = i18n::t("Preview hidden");
        p.drawText(l.text, align, hidden);
        qreal x = l.text.left() + fm.horizontalAdvance(hidden) + 6;
        p.drawText(QRectF(x, 0, w - x, h), align, QString(QChar(0x00b7)));
        x += fm.horizontalAdvance(QString(QChar(0x00b7))) + 6;
        QFont bold = f;
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(pressed ? pal.progress : pal.link);
        p.drawText(QRectF(x, 0, qMax(0.0, w - x - 8), h), align, i18n::t("Show"));
        return out;
    }

    // ---- the panel ----
    if (!onBubble(style)) {
        p.setPen(QPen(pal.border, 1));
        p.setBrush(hovered && card.look != Look::Loading ? pal.backgroundHover : pal.background);
        p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), cornerRadius(), cornerRadius());
    }

    if (!rich(card)) {
        // The glyph on its tinted square, or the ring / the alert.
        QColor square = tint;
        square.setAlpha(style.dark ? 46 : 30);
        p.setPen(Qt::NoPen);
        p.setBrush(card.look == Look::Error ? QColor(pal.errorFill.red(), pal.errorFill.green(), pal.errorFill.blue(), style.dark ? 46 : 30) : square);
        p.drawRoundedRect(l.icon, 8, 8);
        const QRectF glyph = l.icon.adjusted(6, 6, -6, -6);
        if (card.look == Look::Loading)
            drawRing(p, l.icon.center(), 9, 2.2, card.progress, pal.track, pal.progress, style.animate);
        else if (card.look == Look::Error)
            drawAlertIcon(p, glyph.adjusted(1, 1, -1, -1), pal.errorFill, Qt::white);
        else
            drawKindGlyph(p, glyph, card.kind, tint, style);

        // Two lines: the site, then what it is / how far / why not.
        const QFont         titleFont = font(style, 0, true);
        const QFont         subFont   = font(style, -1.5);
        const QFontMetricsF tfm(titleFont);
        const QFontMetricsF sfm(subFont);
        const qreal         block = tfm.height() + 2 + sfm.height();
        const qreal         top   = (h - block) / 2.0;
        p.setFont(titleFont);
        p.setPen(pal.title);
        // A long host loses its start, never its end: the end names whose site it is ("…example.net").
        QString host = tfm.elidedText(card.host, Qt::ElideLeft, l.text.width());
        if (host.size() > 1 && host.at(0) == QChar(0x2026) && host.at(1) == QLatin1Char('.'))
            host.remove(1, 1);
        p.drawText(QRectF(l.text.left(), top, l.text.width(), tfm.height()), align, host);
        p.setFont(subFont);
        p.setPen(card.look == Look::Error ? pal.errorText : pal.muted);
        // From most to least complete; a narrow chat gets a short line that still says it all.
        QStringList status{statusText(card)};
        if (card.look == Look::TooLarge) {
            if (card.size > 0)
                status << i18n::t("Too large · %1").arg(formatSize(static_cast<quint64>(card.size)));
            status << i18n::t("Too large");
        }
        if (card.look == Look::Error) {
            if (!card.error.isEmpty())
                status << i18n::t("Couldn't load the preview");
            status << i18n::t("Couldn't load");
        }
        if (card.look == Look::Loading) {
            // The short forms keep what it waits for (a narrow chat): "Loading…" alone would look like a website load.
            if (card.waiting == Waiting::Own) {
                status << i18n::t("Making preview…");
            } else if (card.waiting == Waiting::Sender) {
                status << i18n::t("Waiting for the sender…") << i18n::t("Sender's preview…");
            } else if (card.waiting == Waiting::Checking) {
                status << i18n::t("Checking for a preview…") << i18n::t("Checking…");
            }
            status << (card.progress >= 0 && card.waiting == Waiting::None ? i18n::t("Loading… %1%").arg(qRound(qBound(0.0, card.progress, 1.0) * 100))
                                                                          : i18n::t("Loading…"));
        }
        if (card.look == Look::Click && (card.size > 0 || (card.overAutoLimit && card.autoLimit > 0)))
            status << kindLabel(card.kind);
        p.drawText(QRectF(l.text.left(), top + tfm.height() + 2, l.text.width(), sfm.height()), align, fitText(sfm, status, l.text.width()));

        // The action on the right: its icon, and its label where there is room.
        if (!l.action.isEmpty()) {
            const bool   on   = pointer.over && (pointer.hover == primaryZone(card) || pointer.hover == Zone::Body);
            const QColor ink  = pressed ? pal.progress : on ? pal.link : pal.muted;
            const QRectF icon(l.action.left(), l.action.center().y() - 9, 18, 18);
            if (card.look == Look::Click)
                drawDownloadIcon(p, icon, ink);
            else if (card.look == Look::Error)
                drawCircularArrow(p, icon, ink);
            else
                drawOpenIcon(p, icon, ink);
            if (l.action.width() > 24) {
                p.setFont(font(style, -1, true));
                p.setPen(on || pressed ? pal.link : pal.title);
                p.drawText(QRectF(icon.right() + 6, l.action.top(), l.action.width() - 24, l.action.height()), align, actionText(card));
            }
        }
    } else {
        const QColor bar = tint;
        if (card.look == Look::YouTube) {
            p.setPen(Qt::NoPen);
            p.setBrush(bar);
            const qreal x = padOf(style);
            p.drawRoundedRect(QRectF(x, onBubble(style) ? 4 : kPad, kBar, h - (onBubble(style) ? 8 : 2 * kPad)), kBar / 2.0, kBar / 2.0);
            // The channel, then the title (two lines at most).
            const QFont         small = font(style, -1.5, true);
            const QFontMetricsF sfm(small);
            const qreal         cw = l.thumb.width();
            p.setFont(small);
            p.setPen(pal.title);
            const QString channel = card.author.isEmpty() ? QString::fromLatin1("YouTube") : card.author;
            p.drawText(QRectF(l.thumb.left(), l.title.top() - sfm.height() - 3, cw - kHide, sfm.height()), align, sfm.elidedText(channel, Qt::ElideRight, cw - kHide));
            const QFont         big = font(style, 0, true);
            const QFontMetricsF bfm(big);
            const QStringList   lines = titleLines(shownTitle(card), big, cw);
            p.setFont(big);
            p.setPen(hovered ? pal.link : pal.title);
            for (int i = 0; i < lines.size(); ++i)
                p.drawText(QRectF(l.title.left(), l.title.top() + i * bfm.lineSpacing(), cw, bfm.lineSpacing()), align, lines.at(i));
        }
        drawThumb(p, card, l, pal, style, hovered, pressed);
        // The site line (and the Poster's action).
        const QFont         foot = font(style, -1.5);
        const QFontMetricsF ffm(foot);
        p.setFont(foot);
        p.setPen(pal.muted);
        const qreal textRight = l.action.isEmpty() ? l.footer.right() - 18 : l.action.left() - kGap;
        const QString site    = card.look == Look::Poster ? card.host + QString::fromUtf8(" · ") + statusText(card) : card.host;
        p.drawText(QRectF(l.footer.left(), l.footer.top(), qMax(0.0, textRight - l.footer.left()), l.footer.height()), align,
                   ffm.elidedText(site, Qt::ElideRight, qMax(0.0, textRight - l.footer.left())));
        if (card.look == Look::YouTube) {
            const qreal x = qMin(l.footer.left() + ffm.horizontalAdvance(card.host) + 4, textRight);
            drawOpenIcon(p, QRectF(x, l.footer.center().y() - 7, 14, 14), hovered ? pal.link : pal.muted);
        } else if (!l.action.isEmpty()) {
            const QColor ink = pressed ? pal.progress : hovered ? pal.link : pal.muted;
            drawOpenIcon(p, QRectF(l.action.left(), l.action.center().y() - 8, 16, 16), ink);
            if (l.action.width() > 24) {
                p.setFont(font(style, -1, true));
                p.setPen(hovered ? pal.link : pal.title);
                p.drawText(QRectF(l.action.left() + 22, l.action.top(), l.action.width() - 22, l.action.height()), align, actionText(card));
            }
        }
    }

    // The hide button, while the pointer is on the card.
    if (pointer.over && card.look != Look::Hidden) {
        const bool   on   = pointer.hover == Zone::Hide;
        const QRectF r    = l.hide;
        const bool   onPoster = card.look == Look::Poster;
        if (on || onPoster) {
            p.setPen(Qt::NoPen);
            p.setBrush(onPoster ? QColor(0, 0, 0, on ? 200 : 150) : pal.placeholderHover);
            p.drawEllipse(r);
        }
        drawCloseGlyph(p, r, onPoster ? QColor(Qt::white) : on ? pal.title : pal.muted);
    }
    return out;
}

QVector<PreviewColorPair> colorPairs(bool dark)
{
    const Palette             pal = palette(dark);
    QVector<PreviewColorPair> out;
    out.append({QStringLiteral("link card site"), pal.title, pal.background, 4.5});
    out.append({QStringLiteral("link card status"), pal.muted, pal.background, 4.5});
    out.append({QStringLiteral("link card status (hover)"), pal.muted, pal.backgroundHover, 4.5});
    out.append({QStringLiteral("link card error"), pal.errorText, pal.background, 4.5});
    out.append({QStringLiteral("link card action (hover)"), pal.link, pal.backgroundHover, 4.5});
    out.append({QStringLiteral("link card hidden pill"), pal.muted, pal.placeholder, 4.5});
    out.append({QStringLiteral("link card show"), pal.link, pal.placeholder, 4.5});
    for (const linkpreview::Kind k : {linkpreview::Kind::YouTube, linkpreview::Kind::Image, linkpreview::Kind::Gif, linkpreview::Kind::Video})
        out.append({QStringLiteral("link card glyph ") + kindLabel(k), tintOf(k, dark), pal.background, 3.0});
    return out;
}

} // namespace linkcard
