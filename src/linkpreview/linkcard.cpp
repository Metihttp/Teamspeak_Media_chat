#include "linkpreview/linkcard.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QtMath>

#include <cmath>
#include <utility>

#include "i18n.h"
#include "linkpreview/providers.h"
#include "linkpreview/textclean.h"
#include "medialink.h"
#include "previewpaint.h"
#include "spoiler.h"
#include "uiutil.h"
#include "ytplayer/ytpage.h" // 2.5 YouTube player: the Consent state's and the notes' texts

using namespace previewpaint;
namespace providers = linkpreview::providers;

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
// 2.5 Embed (9.2)
constexpr qreal kThumbGap        = 12.0;
constexpr qreal kPictureRadius   = 6.0;
constexpr qreal kThumbFallbackMax = 160.0; // a thumbnail picture under the text (no room beside it)
constexpr qreal kDurationPill    = 18.0;
constexpr qint64 kStillAboveMs   = 60000; // a "Site busy" wait longer than this is shown still (no ring)
constexpr int    kEmbedTipChars  = 300;   // an Embed card's title and text in its tool tip

bool rich(const Card& card)
{
    return card.look == Look::YouTube || card.look == Look::Poster;
}

qreal padOf(const PreviewStyle& style)
{
    return onBubble(style) ? kPadOnBubble : kPad;
}

QVector<QColor> backsOf(const PreviewStyle& style, const Palette& pal)
{
    QVector<QColor> backs = style.bubbles;
    if (backs.isEmpty())
        backs.append(pal.background);
    return backs;
}

// The kind's colour (glyph and YouTube's bar): non-text, 3:1 on the card is enough.
QColor tintOf(linkpreview::Kind kind, bool dark)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
        return dark ? QColor(0xff, 0x4e, 0x45) : QColor(0xd9, 0x1e, 0x18);
    case linkpreview::Kind::Gif:
    case linkpreview::Kind::Loop:
        return dark ? QColor(0xc0, 0x84, 0xfc) : QColor(0x86, 0x3b, 0xd6);
    case linkpreview::Kind::Video:
        return dark ? QColor(0x3b, 0xa5, 0x5d) : QColor(0x24, 0x80, 0x45);
    default:
        return dark ? QColor(0x00, 0xa8, 0xfc) : QColor(0x00, 0x6c, 0xe0);
    }
}

// A well-known site's brand (a Site row, or a media page that has one: Tumblr), else none.
providers::Brand brandOfCard(const Card& card)
{
    if (card.provider.isEmpty() || (card.cls != linkpreview::LinkClass::Site && card.cls != linkpreview::LinkClass::MediaPage))
        return {};
    return providers::brandOf(card.provider);
}

QColor readableTint(const Card& card, const PreviewStyle& style, const Palette& pal)
{
    // 2.5: a site's strips wear its brand colour (3:1 on the card).
    const providers::Brand brand = brandOfCard(card);
    if (brand.name && card.kind == linkpreview::Kind::Card)
        return ui::readableOn(QColor(style.dark ? brand.dark : brand.light), backsOf(style, pal), 3.0);
    return ui::readableOn(tintOf(card.kind, style.dark), backsOf(style, pal), 3.0);
}

QString actionText(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        return i18n::t("Load preview");
    case Look::Error:
        return card.nothing ? i18n::t("Open in browser") : i18n::t("Retry");
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

bool coolingStill(const Card& card)
{
    return card.look == Look::Loading && card.waiting == Waiting::Host && card.retryInMs > kStillAboveMs;
}

bool covered(const Card& card)
{
    return card.look == Look::Embed && card.data.sensitive && !card.revealed && card.data.layout != linkpreview::CardLayout::None;
}

QString secondsText(qint64 ms)
{
    return QString::number(qMax<qint64>(1, (ms + 999) / 1000));
}

QString minutesText(qint64 ms)
{
    return QString::number(qMax<qint64>(1, (ms + 59999) / 60000));
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

// 2.5: a page (a rounded window with three lines of text), for card strips.
void drawPageGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal  s = strokeOf(box);
    const QRectF r = box.adjusted(box.width() * 0.14, box.height() * 0.16, -box.width() * 0.14, -box.height() * 0.16);
    p.setPen(roundPen(color, s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 3, 3);
    const qreal top = r.top() + r.height() * 0.22;
    p.drawLine(QPointF(r.left(), top), QPointF(r.right(), top));
    for (int i = 0; i < 3; ++i) {
        const qreal y     = r.top() + r.height() * (0.44 + 0.17 * i);
        const qreal right = i == 2 ? r.left() + r.width() * 0.55 : r.right() - r.width() * 0.18;
        p.drawLine(QPointF(r.left() + r.width() * 0.18, y), QPointF(right, y));
    }
}

void drawKindGlyph(QPainter& p, const QRectF& box, linkpreview::Kind kind, const QColor& color, const PreviewStyle& style)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
    case linkpreview::Kind::Video:
        drawVideoGlyph(p, box, color);
        break;
    case linkpreview::Kind::Gif:
    case linkpreview::Kind::Loop:
        drawGifGlyph(p, box, color, style);
        break;
    case linkpreview::Kind::Card:
        drawPageGlyph(p, box, color);
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

QStringList wrapLines(const QString& text, const QFont& font, qreal width, int maxLines, qreal firstWidth);

// 2.5 YouTube player: a pill in the middle of the thumbnail (white text on a dark pill: 4.5:1 over a white picture).
void drawThumbPill(QPainter& p, const Layout& l, const PreviewStyle& style, const QString& text, bool hovered)
{
    const QFont         f = font(style, -1, true);
    const QFontMetricsF fm(f);
    const qreal         w = qMin(l.thumb.width() - 16, fm.horizontalAdvance(text) + 24);
    const QRectF        pill(l.thumb.center().x() - w / 2.0, l.thumb.center().y() - kPillHeight / 2.0, w, kPillHeight);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered ? QColor(0, 0, 0, 215) : QColor(0, 0, 0, 184));
    p.drawRoundedRect(pill, kPillHeight / 2.0, kPillHeight / 2.0);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(pill.adjusted(12, 0, -12, 0), Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, pill.width() - 24));
}

// A small badge at the thumbnail's bottom right (a poster's length, a YouTube card's time).
void drawBadge(QPainter& p, const QRectF& thumb, const PreviewStyle& style, const QString& text)
{
    const QFont         f = font(style, -2, true);
    const QFontMetricsF fm(f);
    const QRectF        badge(thumb.right() - 8 - fm.horizontalAdvance(text) - 12, thumb.bottom() - 8 - fm.height() - 4, fm.horizontalAdvance(text) + 12,
                              fm.height() + 4);
    p.setPen(Qt::NoPen); // the play glyph's pen would ring the badge (the player's badge has none)
    p.setBrush(QColor(0, 0, 0, 165));
    p.drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(badge, Qt::AlignCenter, text);
}

// The Popup state's pill: "Playing in a window", in a narrow card (160 du) "In a window" (never cut).
QString popupPillTextImpl(const Layout& l, const PreviewStyle& style)
{
    const QFontMetricsF fm(font(style, -1, true));
    const QString       text = i18n::t("Playing in a window");
    return fm.horizontalAdvance(text) + 24 > l.thumb.width() - 16 ? i18n::t("In a window") : text;
}

// The Note state: the thumbnail under a 55% black scrim, why it can't play here (the lines it needs, three at most
// while they fit above the pill) and "Watch on YouTube". In a narrow card (160 du: one line above the pill) the note
// says its short form ("Only on YouTube"), and the pill drops its icon, then says just "YouTube", so nothing is cut.
NoteText noteTextImpl(const Card& card, const Layout& l, const PreviewStyle& style)
{
    NoteText t;
    const QFont         nf = font(style, -1, true);
    const QFontMetricsF pfm(nf);
    const int           fit   = qBound(1, static_cast<int>((l.thumb.height() - 8 - 10 - kPillHeight) / qMax<qreal>(1.0, pfm.height())), 3);
    const QChar         cut(0x2026);
    t.lines = wrapLines(card.note, nf, l.thumb.width() - 16, fit, -1);
    if (t.lines.join(QLatin1Char(' ')).contains(cut) && !card.note.contains(cut)) {
        const QString shorter = ytpage::shortNoteText(card.note);
        if (!shorter.isEmpty())
            t.lines = wrapLines(shorter, nf, l.thumb.width() - 16, fit, -1);
    }
    t.pill     = i18n::t("Watch on YouTube");
    t.pillIcon = true;
    const qreal room = l.thumb.width() - 16;
    if (pfm.horizontalAdvance(t.pill) + 14 + 6 + 24 > room) {
        t.pillIcon = false;
        if (pfm.horizontalAdvance(t.pill) + 24 > room)
            t.pill = i18n::t("YouTube");
    }
    t.pillWidth = qMin(room, pfm.horizontalAdvance(t.pill) + (t.pillIcon ? 14 + 6 : 0) + 24);
    return t;
}

void drawNote(QPainter& p, const Card& card, const Layout& l, const PreviewStyle& style, const QPainterPath& clip, bool hovered)
{
    p.save();
    p.setClipPath(clip);
    p.fillRect(l.thumb, QColor(0, 0, 0, 140));
    p.restore();
    const QFont         nf = font(style, -1, true);
    const QFontMetricsF nfm(nf);
    const QFont         pf = font(style, -1, true);
    const QFontMetricsF pfm(pf);
    const NoteText      text  = noteTextImpl(card, l, style);
    const qreal         lines = nfm.height() * text.lines.size();
    const qreal         block = lines + 10 + kPillHeight;
    const qreal         top   = l.thumb.center().y() - block / 2.0;
    p.setFont(nf);
    p.setPen(Qt::white);
    for (int i = 0; i < text.lines.size(); ++i)
        p.drawText(QRectF(l.thumb.left() + 8, top + i * nfm.height(), l.thumb.width() - 16, nfm.height()), Qt::AlignCenter, text.lines.at(i));
    const QRectF pill(l.thumb.center().x() - text.pillWidth / 2.0, top + lines + 10, text.pillWidth, kPillHeight);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered ? QColor(255, 255, 255, 240) : QColor(255, 255, 255, 220));
    p.drawRoundedRect(pill, kPillHeight / 2.0, kPillHeight / 2.0);
    const QColor ink(0x1f, 0x1f, 0x1f);
    const qreal  icon = text.pillIcon ? 14 + 6 : 0;
    if (text.pillIcon)
        drawOpenIcon(p, QRectF(pill.left() + 12, pill.center().y() - 7, 14, 14), ink);
    p.setFont(pf);
    p.setPen(ink);
    p.drawText(pill.adjusted(12 + icon, 0, -12, 0), (text.pillIcon ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter | Qt::AlignAbsolute,
               pfm.elidedText(text.pill, Qt::ElideRight, pill.width() - 12 - icon - 12));
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
    const bool yt = card.look == Look::YouTube;
    if (yt && card.player == Player::Consent)
        return; // the notice is drawn over it: no disc
    if (yt && card.player == Player::Note) {
        drawNote(p, card, l, style, clip, hovered);
        return;
    }
    if (card.thumbnail.isNull() && card.thumbnailLoading && !(yt && card.player != Player::None)) {
        drawRing(p, l.thumb.center(), 12, 2.5, -1.0, pal.track, pal.progress, style.animate);
        return;
    }
    if (yt && card.player == Player::Popup) {
        // The player is in its own window: the thumbnail says so (a click brings that window to the front).
        drawThumbPill(p, l, style, popupPillTextImpl(l, style), hovered);
        return;
    }
    // The play disc: dark and translucent like the video player's (white glyph, 4.5:1 on it over white).
    const QColor disc = pressed ? QColor(0, 0, 0, 215) : hovered ? QColor(0, 0, 0, 190) : QColor(0, 0, 0, 165);
    p.setPen(Qt::NoPen);
    p.setBrush(disc);
    p.drawEllipse(l.play);
    if (yt && card.player == Player::Starting)
        drawRing(p, l.play.center(), l.play.width() * 0.3, 2.5, -1.0, QColor(255, 255, 255, 70), Qt::white, style.animate); // the player starts
    else
        drawPlayIcon(p, l.play.adjusted(l.play.width() * 0.3, l.play.height() * 0.3, -l.play.width() * 0.26, -l.play.height() * 0.3), Qt::white);
    if (yt && card.resumeMs > 0 && (card.player == Player::None || card.player == Player::Hidden))
        drawBadge(p, l.thumb, style, ytpage::timeText(card.resumeMs)); // where it stopped (Play goes on from there)
    if (card.look == Look::Poster && card.durationMs > 0)
        drawBadge(p, l.thumb, style, formatDuration(card.durationMs));
}

QString shownTitle(const Card& card)
{
    return card.title.isEmpty() ? kindLabel(card.kind) : card.title;
}

// Text in lines: the lines it needs, at most maxLines (the last one elided); the first line may be narrower (room
// for the hide button). A card gets its text with its look, so its height never changes while it is shown.
QStringList wrapLines(const QString& text, const QFont& font, qreal width, int maxLines, qreal firstWidth = -1)
{
    if (text.isEmpty() || maxLines <= 0)
        return {};
    QTextLayout tl(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setTextDirection(text.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight);
    tl.setTextOption(option);
    tl.beginLayout();
    QStringList lines;
    int         lastStart = 0;
    bool        more      = false;
    for (int i = 0; i <= maxLines; ++i) {
        QTextLine line = tl.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(i == 0 && firstWidth > 0 ? firstWidth : width);
        if (i == maxLines) {
            more = true;
            break;
        }
        lastStart = line.textStart();
        lines << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    tl.endLayout();
    if (more && !lines.isEmpty()) {
        const qreal last = lines.size() == 1 && firstWidth > 0 ? firstWidth : width;
        lines.last()     = QFontMetricsF(font).elidedText(text.mid(lastStart).simplified() + QChar(0x2026), Qt::ElideRight, last);
    }
    return lines;
}

// A YouTube title as the card shows it: the lines it needs, at most two (the second elided).
QStringList titleLines(const QString& title, const QFont& font, qreal width)
{
    QStringList lines = wrapLines(title, font, width, 2);
    if (lines.isEmpty())
        lines << QString();
    return lines;
}

// One line of page text in its paragraph's reading direction (rtl: decided once from the whole paragraph, so a
// Persian title's line that starts with a Latin word still reads right to left and stays right-aligned), clipped to
// its box (combining marks can't paint outside it).
void drawLine(QPainter& p, const QRectF& rect, const QString& text, bool rtl)
{
    if (text.isEmpty() || rect.isEmpty())
        return;
    const Qt::Alignment align = Qt::AlignVCenter | Qt::AlignAbsolute | (rtl ? Qt::AlignRight : Qt::AlignLeft);
    p.save();
    p.setClipRect(rect);
    p.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight); // the paragraph's own base direction (word order)
    p.drawText(rect, align, text);
    p.restore();
}

QFont siteFont(const Card& card, const PreviewStyle& style)
{
    QFont f = font(style, -1.5);
    if (brandOfCard(card).name)
        f.setWeight(QFont::DemiBold); // a well-known site's brand line: a style no web page's site line gets
    return f;
}

QString siteTextOf(const Card& card)
{
    const providers::Brand brand = brandOfCard(card);
    if (brand.name)
        return QString::fromUtf8(brand.name); // never from a page
    return card.data.site;
}

// The Embed card's geometry (9.2).
Layout embedLayout(const Card& card, const PreviewStyle& style)
{
    Layout      l;
    const bool  bubble = onBubble(style);
    const int   w      = qMin(kEmbedWidth, qMax(kMinWidth, style.maxWidth));
    const qreal pad    = bubble ? 2 : 12;
    const qreal top    = bubble ? 4 : 12;
    const qreal bottom = bubble ? 4 : 10;
    const qreal x0     = pad + kBar + 10;
    const qreal cw     = qMax(40.0, w - x0 - pad);
    linkpreview::CardLayout lay = card.data.layout;
    if (lay == linkpreview::CardLayout::Large && card.slotIndex > 0)
        lay = linkpreview::CardLayout::Thumb; // only a message's first card is large
    l.pictureLayout          = lay;
    const bool  hasPicture   = lay != linkpreview::CardLayout::None;
    l.thumbSide              = lay == linkpreview::CardLayout::Thumb && hasPicture && cw >= kEmbedMinThumbColumn;
    const qreal tw           = l.thumbSide ? cw - kEmbedThumb - kThumbGap : cw;
    const qreal firstWidth   = l.thumbSide ? tw : tw - 24;
    if (l.thumbSide)
        l.hide = QRectF(x0 + cw - kHide - 2, top + 2, kHide, kHide); // on the thumbnail's top-right corner
    else
        l.hide = QRectF(w - (bubble ? 0 : 6) - kHide, top - 2, kHide, kHide);
    qreal y     = top;
    bool  first = true;
    const auto widthFor = [&](void) {
        const qreal width = first ? firstWidth : tw;
        first             = false;
        return width;
    };
    l.siteText = siteTextOf(card);
    if (!l.siteText.isEmpty()) {
        const QFontMetricsF fm(siteFont(card, style));
        l.siteRtl  = l.siteText.isRightToLeft();
        l.site     = QRectF(x0, y, widthFor(), fm.height());
        l.siteText = fm.elidedText(l.siteText, Qt::ElideRight, l.site.width());
        y += fm.height() + 2;
    }
    if (!card.data.author.isEmpty()) {
        const QFontMetricsF fm(font(style, -1.5, true));
        l.authorRtl  = card.data.author.isRightToLeft();
        l.author     = QRectF(x0, y, widthFor(), fm.height());
        l.authorText = fm.elidedText(card.data.author, Qt::ElideRight, l.author.width());
        y += fm.height() + 4;
    }
    // A paragraph's line boxes: its first line as narrow as it was wrapped when it is the card's first text line.
    const auto lineRects = [&](int count, qreal top, qreal spacing, qreal firstLine) {
        QVector<QRectF> rects;
        for (int i = 0; i < count; ++i)
            rects << QRectF(x0, top + i * spacing, i == 0 && firstLine > 0 ? qMin(firstLine, tw) : tw, spacing);
        return rects;
    };
    if (!card.data.title.isEmpty()) {
        const QFont         f = font(style, 0, true);
        const QFontMetricsF fm(f);
        const qreal         firstLine = first ? firstWidth : -1;
        l.titleRtl   = card.data.title.isRightToLeft();
        l.titleLines = wrapLines(card.data.title, f, tw, 2, firstLine);
        first        = false;
        l.title      = QRectF(x0, y, tw, l.titleLines.size() * fm.lineSpacing());
        l.titleRects = lineRects(l.titleLines.size(), y, fm.lineSpacing(), firstLine);
        y += l.titleLines.size() * fm.lineSpacing() + 4;
    }
    if (!card.data.description.isEmpty()) {
        const QFont         f = font(style, -1);
        const QFontMetricsF fm(f);
        const qreal         firstLine = first ? firstWidth : -1;
        l.descriptionRtl   = card.data.description.isRightToLeft();
        l.descriptionLines = wrapLines(card.data.description, f, tw, w < kEmbedNarrow ? 2 : 3, firstLine);
        first              = false;
        l.description      = QRectF(x0, y, tw, l.descriptionLines.size() * fm.lineSpacing());
        l.descriptionRects = lineRects(l.descriptionLines.size(), y, fm.lineSpacing(), firstLine);
        y += l.descriptionLines.size() * fm.lineSpacing();
    }
    const qreal textBottom = y;
    if (l.thumbSide) {
        l.thumb = QRectF(x0 + cw - kEmbedThumb, top, kEmbedThumb, kEmbedThumb);
        y       = qMax(textBottom, top + kEmbedThumb);
    } else if (hasPicture) {
        y += 8;
        const int   pw     = card.data.pictureWidth;
        const int   ph     = card.data.pictureHeight;
        const qreal aspect = pw > 0 && ph > 0 ? qBound(0.75, static_cast<qreal>(pw) / ph, 2.4) : 16.0 / 9.0;
        const qreal most   = lay == linkpreview::CardLayout::Thumb ? kThumbFallbackMax : static_cast<qreal>(kEmbedMaxPicture);
        const qreal height = qMin<qreal>(std::round(cw / aspect), most);
        l.thumb            = QRectF(x0, y, cw, height);
        y += height;
    }
    if (!l.thumb.isEmpty() && card.data.play) {
        const qreal disc = qMin(kPlayDisc, qMin(l.thumb.width(), l.thumb.height()) * 0.6);
        l.play           = QRectF(l.thumb.center().x() - disc / 2.0, l.thumb.center().y() - disc / 2.0, disc, disc);
    }
    y += 8;
    const QFontMetricsF foot(font(style, -1.5));
    l.footer = QRectF(x0, y, cw, qMax(foot.height(), 20.0));
    y += l.footer.height();
    l.size = QSize(w, qCeil(y + bottom));
    l.bar  = QRectF(pad, top, kBar, qMax(0.0, l.size.height() - bottom - top));
    return l;
}

// The host of a strip or an embed's footer: a long one loses its start, never its end ("…example.net").
QString elidedHost(const QFontMetricsF& fm, const QString& host, qreal width)
{
    QString out = fm.elidedText(host, Qt::ElideLeft, width);
    if (out.size() > 1 && out.at(0) == QChar(0x2026) && out.at(1) == QLatin1Char('.'))
        out.remove(1, 1);
    return out;
}

void renderEmbed(QPainter& p, const Card& card, const Layout& l, const Palette& pal, const PreviewStyle& style, const Pointer& pointer, bool hovered,
                 bool pressed)
{
    // The bar.
    p.setPen(Qt::NoPen);
    p.setBrush(barColour(card, style));
    p.drawRoundedRect(l.bar, kBar / 2.0, kBar / 2.0);
    // The site line, the author, the title, the text: each its own paragraph, in its own direction, clipped.
    if (!l.siteText.isEmpty()) {
        p.setFont(siteFont(card, style));
        p.setPen(brandOfCard(card).name ? brandLineColour(card, style) : pal.muted);
        drawLine(p, l.site, l.siteText, l.siteRtl);
    }
    if (!l.authorText.isEmpty()) {
        p.setFont(font(style, -1.5, true));
        p.setPen(pal.title);
        drawLine(p, l.author, l.authorText, l.authorRtl);
    }
    // A paragraph's lines in its own direction, each in its own box (the card's first text line as narrow as it was
    // wrapped: a right-aligned Persian line never runs under the hide button).
    if (!l.titleLines.isEmpty()) {
        p.setFont(font(style, 0, true));
        p.setPen(pressed ? pal.progress : hovered ? pal.link : pal.title);
        for (int i = 0; i < l.titleLines.size() && i < l.titleRects.size(); ++i)
            drawLine(p, l.titleRects.at(i), l.titleLines.at(i), l.titleRtl);
    }
    if (!l.descriptionLines.isEmpty()) {
        p.setFont(font(style, -1));
        p.setPen(pal.title);
        for (int i = 0; i < l.descriptionLines.size() && i < l.descriptionRects.size(); ++i)
            drawLine(p, l.descriptionRects.at(i), l.descriptionLines.at(i), l.descriptionRtl);
    }
    // The picture: covering its box, rounded; a placeholder (with a ring while it comes) until it is there.
    if (!l.thumb.isEmpty()) {
        QPainterPath clip;
        clip.addRoundedRect(l.thumb, kPictureRadius, kPictureRadius);
        p.save();
        p.setClipPath(clip);
        p.fillRect(l.thumb, pal.placeholder);
        if (covered(card)) {
            spoiler::CoverLook look;
            look.font        = font(style, -1, true);
            look.placeholder = pal.placeholder;
            look.hovered     = pointer.over && pointer.hover == Zone::Reveal;
            look.pressed     = look.hovered && pointer.pressed == Zone::Reveal;
            look.withPill    = l.thumb.width() >= 120;
            spoiler::drawCover(p, l.thumb, spoiler::blurredStill(card.picture, false), false, look);
        } else if (!card.picture.isNull()) {
            const QSizeF scaled = QSizeF(card.picture.size()).scaled(l.thumb.size(), Qt::KeepAspectRatioByExpanding);
            p.drawImage(QRectF(l.thumb.center().x() - scaled.width() / 2.0, l.thumb.center().y() - scaled.height() / 2.0, scaled.width(), scaled.height()),
                        card.picture);
        }
        p.restore();
        if (card.picture.isNull() && card.pictureLoading && !covered(card))
            drawRing(p, l.thumb.center(), qMin(12.0, l.thumb.height() / 4.0), 2.5, -1.0, pal.track, pal.progress, style.animate);
        if (!l.play.isEmpty() && !covered(card)) {
            p.setPen(Qt::NoPen);
            p.setBrush(pressed ? QColor(0, 0, 0, 215) : hovered ? QColor(0, 0, 0, 190) : QColor(0, 0, 0, 165));
            p.drawEllipse(l.play);
            drawPlayIcon(p, l.play.adjusted(l.play.width() * 0.3, l.play.height() * 0.3, -l.play.width() * 0.26, -l.play.height() * 0.3), Qt::white);
        }
        if (card.data.play && card.data.durationMs > 0 && !covered(card) && l.thumb.width() >= 120) {
            const QFont         f = font(style, -2, true);
            const QFontMetricsF fm(f);
            const QString       d = formatDuration(card.data.durationMs);
            const QRectF        pill(l.thumb.right() - 6 - fm.horizontalAdvance(d) - 12, l.thumb.bottom() - 6 - kDurationPill, fm.horizontalAdvance(d) + 12,
                                     kDurationPill);
            if (!pill.intersects(l.play.adjusted(-4, -4, 4, 4))) { // a small picture: the play symbol alone
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 184));
                p.drawRoundedRect(pill, kDurationPill / 2.0, kDurationPill / 2.0);
                p.setFont(f);
                p.setPen(Qt::white);
                p.drawText(pill, Qt::AlignCenter, d);
            }
        }
    }
    // The host (its own left-to-right run, never with page text) and the open icon after it.
    const QFont         foot = font(style, -1.5);
    const QFontMetricsF ffm(foot);
    p.setFont(foot);
    p.setPen(pal.muted);
    const qreal   room = qMax(0.0, l.footer.width() - 18);
    const QString host = footerText(card, ffm, room);
    p.drawText(QRectF(l.footer.left(), l.footer.top(), room, l.footer.height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, host);
    const qreal x = qMin(l.footer.left() + ffm.horizontalAdvance(host) + 4, l.footer.right() - 14);
    drawOpenIcon(p, QRectF(x, l.footer.center().y() - 7, 14, 14), hovered ? pal.link : pal.muted);
}

// 2.5 YouTube player: the Consent state's parts, laid out inside the card's own size (sizes never depend on state):
// the question (two lines at most), from 280 du the short notice (three), the Terms and Privacy links (one row, or
// two), the pills Play and Open in browser (side by side, or stacked). What doesn't fit goes, the notice first.
void consentLayout(const Card& card, const PreviewStyle& style, Layout& l)
{
    Q_UNUSED(card);
    const qreal w = l.size.width(), h = l.size.height();
    l.consentBox  = QRectF(0.5, 0.5, w - 1, h - 1);
    const QRectF        inner = l.consentBox.adjusted(18, 8, -18, -8);
    const QFont         qf    = font(style, 0, true);
    const QFont         nf    = font(style, -1.5);
    const QFont         lf    = font(style, -1.5, true);
    const QFont         pf    = font(style, -1, true);
    const QFontMetricsF qfm(qf), nfm(nf), lfm(lf), pfm(pf);
    l.questionLines           = wrapLines(ytpage::consentQuestion(), qf, inner.width(), 2);
    l.noticeLines             = w >= kEmbedNarrow ? wrapLines(ytpage::consentLine(), nf, inner.width(), 3) : QStringList();
    const qreal termsW        = lfm.horizontalAdvance(ytpage::termsText());
    const qreal privacyW      = lfm.horizontalAdvance(ytpage::privacyText());
    const qreal dotW          = lfm.horizontalAdvance(QString::fromUtf8(" \xC2\xB7 "));
    l.linksStacked            = termsW + dotW + privacyW > inner.width();
    const qreal playW         = qMin(inner.width(), 12 + 10 + 6 + pfm.horizontalAdvance(i18n::t("Play")) + 14);
    const qreal browserW      = qMin(inner.width(), 14 + pfm.horizontalAdvance(i18n::t("Open in browser")) + 14);
    l.pillsStacked            = playW + 8 + browserW > inner.width();
    const auto total = [&]() {
        qreal t = l.questionLines.size() * qfm.lineSpacing();
        if (!l.noticeLines.isEmpty())
            t += 6 + l.noticeLines.size() * nfm.lineSpacing();
        t += 8 + (l.linksStacked ? 2 : 1) * lfm.lineSpacing();
        t += 10 + (l.pillsStacked ? 2 * kPillHeight + 6 : kPillHeight);
        return t;
    };
    while (total() > inner.height() && !l.noticeLines.isEmpty())
        l.noticeLines.removeLast();
    if (total() > inner.height() && l.questionLines.size() > 1)
        l.questionLines = QStringList{qfm.elidedText(ytpage::consentQuestion(), Qt::ElideRight, inner.width())};
    qreal y = inner.top() + qMax<qreal>(0, (inner.height() - total()) / 2.0);
    l.question = QRectF(inner.left(), y, inner.width(), l.questionLines.size() * qfm.lineSpacing());
    y          = l.question.bottom();
    if (!l.noticeLines.isEmpty()) {
        l.notice = QRectF(inner.left(), y + 6, inner.width(), l.noticeLines.size() * nfm.lineSpacing());
        y        = l.notice.bottom();
    } else {
        l.notice = QRectF();
    }
    y += 8;
    const qreal tw = qMin(termsW, inner.width()), pw = qMin(privacyW, inner.width());
    if (l.linksStacked) {
        l.terms   = QRectF(inner.center().x() - tw / 2.0, y, tw, lfm.lineSpacing());
        l.privacy = QRectF(inner.center().x() - pw / 2.0, y + lfm.lineSpacing(), pw, lfm.lineSpacing());
        y += 2 * lfm.lineSpacing();
    } else {
        const qreal x0 = inner.center().x() - (termsW + dotW + privacyW) / 2.0;
        l.terms        = QRectF(x0, y, termsW, lfm.lineSpacing());
        l.privacy      = QRectF(x0 + termsW + dotW, y, privacyW, lfm.lineSpacing());
        y += lfm.lineSpacing();
    }
    y += 10;
    if (l.pillsStacked) {
        l.consentPlay    = QRectF(inner.center().x() - playW / 2.0, y, playW, kPillHeight);
        l.consentBrowser = QRectF(inner.center().x() - browserW / 2.0, y + kPillHeight + 6, browserW, kPillHeight);
    } else {
        const qreal x0   = inner.center().x() - (playW + 8 + browserW) / 2.0;
        l.consentPlay    = QRectF(x0, y, playW, kPillHeight);
        l.consentBrowser = QRectF(x0 + playW + 8, y, browserW, kPillHeight);
    }
}

// The Consent state over the drawn card: a 55% black scrim (white text on it: 4.5:1 over a white card), the question,
// the notice, the links (underlined) and the two pills.
void drawConsent(QPainter& p, const Layout& l, const PreviewStyle& style, const Pointer& pointer)
{
    QPainterPath box;
    box.addRoundedRect(l.consentBox, cornerRadius(), cornerRadius());
    p.fillPath(box, QColor(0, 0, 0, 140));
    const QFont         qf = font(style, 0, true);
    const QFont         nf = font(style, -1.5);
    QFont               lf = font(style, -1.5, true);
    const QFont         pf = font(style, -1, true);
    const QFontMetricsF qfm(qf), nfm(nf), lfm(lf), pfm(pf);
    p.setPen(Qt::white);
    p.setFont(qf);
    for (int i = 0; i < l.questionLines.size(); ++i)
        p.drawText(QRectF(l.question.left(), l.question.top() + i * qfm.lineSpacing(), l.question.width(), qfm.lineSpacing()), Qt::AlignCenter,
                   l.questionLines.at(i));
    p.setFont(nf);
    p.setPen(QColor(255, 255, 255, 235));
    for (int i = 0; i < l.noticeLines.size(); ++i)
        p.drawText(QRectF(l.notice.left(), l.notice.top() + i * nfm.lineSpacing(), l.notice.width(), nfm.lineSpacing()), Qt::AlignCenter,
                   l.noticeLines.at(i));
    const auto over = [&pointer](Zone z) { return pointer.over && pointer.hover == z; };
    lf.setUnderline(true);
    p.setFont(lf);
    p.setPen(Qt::white);
    QFontMetricsF ufm(lf);
    p.drawText(l.terms, Qt::AlignCenter, ufm.elidedText(ytpage::termsText(), Qt::ElideRight, l.terms.width() + 1));
    p.drawText(l.privacy, Qt::AlignCenter, ufm.elidedText(ytpage::privacyText(), Qt::ElideRight, l.privacy.width() + 1));
    if (!l.linksStacked) {
        QFont plain = lf;
        plain.setUnderline(false);
        p.setFont(plain);
        p.drawText(QRectF(l.terms.right(), l.terms.top(), l.privacy.left() - l.terms.right(), l.terms.height()), Qt::AlignCenter, QString::fromUtf8("\xC2\xB7"));
    }
    // Play: a white pill with dark text; Open in browser: a white outline.
    const QColor ink(0x1f, 0x1f, 0x1f);
    p.setPen(Qt::NoPen);
    p.setBrush(over(Zone::ConsentPlay) ? QColor(255, 255, 255) : QColor(255, 255, 255, 230));
    p.drawRoundedRect(l.consentPlay, kPillHeight / 2.0, kPillHeight / 2.0);
    const QRectF tri(l.consentPlay.left() + 12, l.consentPlay.center().y() - 5, 10, 10);
    drawPlayIcon(p, tri, ink);
    p.setFont(pf);
    p.setPen(ink);
    p.drawText(QRectF(tri.right() + 6, l.consentPlay.top(), l.consentPlay.right() - tri.right() - 6 - 10, kPillHeight),
               Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, pfm.elidedText(i18n::t("Play"), Qt::ElideRight, l.consentPlay.right() - tri.right() - 16));
    p.setPen(QPen(over(Zone::ConsentBrowser) ? QColor(Qt::white) : QColor(255, 255, 255, 210), 1.5));
    p.setBrush(over(Zone::ConsentBrowser) ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 0));
    p.drawRoundedRect(l.consentBrowser.adjusted(0.75, 0.75, -0.75, -0.75), kPillHeight / 2.0, kPillHeight / 2.0);
    p.setPen(Qt::white);
    p.drawText(l.consentBrowser.adjusted(12, 0, -12, 0), Qt::AlignCenter, pfm.elidedText(i18n::t("Open in browser"), Qt::ElideRight, l.consentBrowser.width() - 24));
}

} // namespace

NoteText noteTextFor(const Card& card, const Layout& l, const PreviewStyle& style)
{
    return noteTextImpl(card, l, style);
}

QString popupPillText(const Layout& l, const PreviewStyle& style)
{
    return popupPillTextImpl(l, style);
}

// ---- texts ----------------------------------------------------------------------------------------------

QString kindLabel(linkpreview::Kind kind)
{
    switch (kind) {
    case linkpreview::Kind::YouTube:
        return i18n::t("YouTube video");
    case linkpreview::Kind::Image:
        return i18n::t("Picture");
    case linkpreview::Kind::Gif:
    case linkpreview::Kind::Loop:
        return i18n::t("GIF");
    case linkpreview::Kind::Video:
        return i18n::t("Video");
    case linkpreview::Kind::Card:
        return i18n::t("Web page");
    case linkpreview::Kind::None:
        break;
    }
    return {};
}

QString stripLabel(const Card& card)
{
    if (card.cls == linkpreview::LinkClass::MediaPage && card.provider == QLatin1String("nextcloud"))
        return i18n::t("Shared file"); // a Nextcloud or ownCloud share: no brand, the host on the site line
    if (card.cls == linkpreview::LinkClass::Site) {
        const providers::Brand brand = providers::brandOf(card.provider);
        if (brand.name)
            return QString::fromUtf8(brand.name);
    }
    if (card.cls == linkpreview::LinkClass::Web && card.kind == linkpreview::Kind::Card)
        return i18n::t("Web page");
    return kindLabel(card.kind);
}

QColor barColour(const Card& card, const PreviewStyle& style)
{
    const Palette          pal   = palette(style);
    const providers::Brand brand = brandOfCard(card);
    if (brand.name)
        return ui::readableOn(QColor(style.dark ? brand.dark : brand.light), backsOf(style, pal), 3.0);
    if (providers::cardsFollowWebRules(card.cls, card.provider) && card.data.hasColor)
        return ui::readableOn(QColor(card.data.color), backsOf(style, pal), 3.0); // a web page's (or a share page's) theme colour
    return style.dark ? QColor(0x4a, 0x4d, 0x55) : QColor(0xc9, 0xcd, 0xd3); // decorative: no site colour
}

QColor brandLineColour(const Card& card, const PreviewStyle& style)
{
    const Palette          pal   = palette(style);
    const providers::Brand brand = brandOfCard(card);
    if (!brand.name)
        return pal.muted;
    return ui::readableOn(QColor(style.dark ? brand.dark : brand.light), backsOf(style, pal), 4.5); // text: 4.5:1
}

QString cleanText(const QString& text, int maxChars)
{
    return linkpreview::textclean::clean(text, maxChars);
}

QString footerText(const Card& card, const QFontMetricsF& fm, qreal width)
{
    // The link's own host (what a click opens) first and whole when it fits; the host a redirect went to only after
    // it, shortened (its end kept) or left out when there is no room.
    const QString own = elidedHost(fm, card.host, width);
    if (card.redirectedTo.isEmpty() || own != card.host)
        return own;
    const QString arrow = QString::fromUtf8(" \xE2\x86\x92 ");
    const qreal   rest  = width - fm.horizontalAdvance(own + arrow);
    if (rest < fm.horizontalAdvance(QString(QChar(0x2026)) + QStringLiteral("abc")))
        return own;
    return own + arrow + elidedHost(fm, card.redirectedTo, rest);
}

QString statusText(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        if (card.size > 0)
            return i18n::t("%1 · %2").arg(stripLabel(card), formatSize(static_cast<quint64>(card.size)));
        if (card.overAutoLimit && card.autoLimit > 0) // the site didn't say how large: at least the limit
            return i18n::t("%1 · over %2").arg(stripLabel(card), formatSize(static_cast<quint64>(card.autoLimit)));
        return stripLabel(card);
    case Look::Loading:
        if (card.waiting == Waiting::Own)
            return i18n::t("Making the preview…");
        if (card.waiting == Waiting::Sender)
            return i18n::t("Waiting for the sender's preview…");
        if (card.waiting == Waiting::Checking)
            return i18n::t("Checking for the sender's preview…");
        if (card.waiting == Waiting::Host) {
            if (coolingStill(card))
                return i18n::t("Site busy · try again in %1 min").arg(minutesText(card.retryInMs));
            return i18n::t("Site busy · retrying in %1 s").arg(secondsText(card.retryInMs));
        }
        return card.progress >= 0 ? i18n::t("Loading preview… %1%").arg(qRound(qBound(0.0, card.progress, 1.0) * 100)) : i18n::t("Loading preview…");
    case Look::Error:
        if (card.nothing)
            return i18n::t("No preview for this page");
        return card.error.isEmpty() ? i18n::t("Couldn't load the preview") : card.error;
    case Look::TooLarge:
        return card.size > 0 ? i18n::t("Too large to preview here · %1").arg(formatSize(static_cast<quint64>(card.size))) : i18n::t("Too large to preview here");
    case Look::YouTube:
        return card.title;
    case Look::Poster:
        return i18n::t("Too large to play here");
    case Look::Hidden:
        return i18n::t("Preview hidden");
    case Look::Embed:
        return card.data.title;
    case Look::Absent:
        break;
    }
    return {};
}

QString toolTip(const Card& card, Zone zone)
{
    // Whom a click on Load preview / Retry contacts: the link's site for a file; for YouTube never the link's
    // own address (youtu.be, m.youtube.com, ...) but YouTube's (www.youtube.com, i.ytimg.com).
    const QString site = card.kind == linkpreview::Kind::YouTube ? QString::fromLatin1("YouTube") : card.host;
    const bool    page = card.cls == linkpreview::LinkClass::MediaPage || card.cls == linkpreview::LinkClass::Site || card.cls == linkpreview::LinkClass::Web;
    if (card.look == Look::YouTube) {
        // 2.5 YouTube player
        switch (zone) {
        case Zone::Play:
            if (card.player == Player::Hidden)
                return i18n::t("Resume the video");
            if (card.player == Player::Popup)
                return i18n::t("Show the player window");
            if constexpr (ytpage::kAnchoredPlayer)
                return i18n::t("Play here");
            else
                return i18n::t("Play in a window"); // 2.5.0: always the popup (the release gate)
        case Zone::Stop:
            return i18n::t("Stop video");
        case Zone::ConsentPlay:
            if constexpr (ytpage::kAnchoredPlayer)
                return i18n::t("Play this video here with YouTube's own player");
            else
                return i18n::t("Play this video in a window with YouTube's own player"); // review fix: the release gate
        case Zone::ConsentBrowser:
            return i18n::t("Open the video in your browser instead");
        case Zone::Terms:
            return i18n::t("Opens %1 in your browser").arg(ytpage::termsUrl().host());
        case Zone::Privacy:
            return i18n::t("Opens %1 in your browser").arg(ytpage::privacyUrl().host());
        case Zone::Body:
            if (card.player == Player::Consent)
                return ytpage::consentNotice();
            if (card.player == Player::Starting)
                return i18n::t("Starting the player\xE2\x80\xA6");
            break;
        default:
            break;
        }
    }
    switch (zone) {
    case Zone::Play:
    case Zone::Stop:
    case Zone::ConsentPlay:
    case Zone::ConsentBrowser:
    case Zone::Terms:
    case Zone::Privacy:
        return {};
    case Zone::Hide:
        return i18n::t("Hide preview");
    case Zone::Show:
        return i18n::t("Show the preview again");
    case Zone::Reveal:
        return i18n::t("Marked as sensitive. Click the picture to show it.");
    case Zone::Open:
        // A YouTube card is one Open zone (zoneAt): its tip is the card's own, with the whole title and channel the
        // card may cut short (live test: the title never showed). 2.5: an Embed card's too.
        if (card.look == Look::YouTube || card.look == Look::Embed)
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
        if (card.cls == linkpreview::LinkClass::MediaPage && card.provider == QLatin1String("nextcloud"))
            return i18n::t("Loading the preview connects your PC to %1, which sees your IP address.").arg(card.host);
        if (card.cls == linkpreview::LinkClass::MediaPage)
            return i18n::t("Loading the preview connects your PC to %1's servers, which see your IP address.").arg(card.host);
        if (card.cls == linkpreview::LinkClass::Site || card.cls == linkpreview::LinkClass::Web)
            return i18n::t("Loading the preview connects your PC to %1 and to the site its picture comes from, which see your IP address.").arg(card.host);
        return i18n::t("Loading the preview connects your PC to %1, which sees your IP address.").arg(site);
    case Look::Loading:
        if (card.waiting == Waiting::Own)
            return i18n::t("Your TS Media is loading this link once to share its preview in this chat.");
        if (card.waiting == Waiting::Sender)
            return i18n::t("The sender's TS Media is sharing a preview of this link through the TeamSpeak server. Your PC doesn't contact %1 for it.").arg(site);
        if (card.waiting == Waiting::Checking)
            return i18n::t("Checking whether the sender's TS Media shares a preview of this link. If it doesn't, the preview may be loaded from %1, which sees your IP address.").arg(site);
        if (card.waiting == Waiting::Host) {
            const QString when = card.retryInMs > kStillAboveMs ? i18n::t("%1 minutes").arg(minutesText(card.retryInMs))
                                                                : i18n::t("%1 seconds").arg(secondsText(card.retryInMs));
            return i18n::t("%1 asked to wait before more previews are loaded from it. This one loads by itself in about %2.")
                .arg(card.busySite.isEmpty() ? card.host : card.busySite, when);
        }
        return i18n::t("Loading the preview from %1…").arg(site);
    case Look::Error:
        if (card.nothing)
            return i18n::t("The page has no preview. Opens %1 in your browser.").arg(card.host);
        if (page && zone == Zone::Body)
            return i18n::t("Loading the preview connects your PC to %1 and to the site its picture comes from, which see your IP address.").arg(card.host);
        return card.error.isEmpty() ? i18n::t("The preview couldn't be loaded from %1.").arg(site) : card.error;
    case Look::TooLarge:
        return i18n::t("This file is too large to preview in the chat. Open it in your browser instead.");
    case Look::YouTube: {
        QString tip = card.title.isEmpty() ? kindLabel(card.kind) : card.title;
        if (!card.author.isEmpty())
            tip += QLatin1Char('\n') + card.author;
        if (card.player == Player::Note && !card.note.isEmpty())
            tip += QLatin1Char('\n') + card.note;
        return tip + QLatin1Char('\n') + i18n::t("Opens on %1 in your browser").arg(card.host);
    }
    case Look::Poster:
        return i18n::t("This video is too large to play in the chat. Opens on %1 in your browser.").arg(card.host);
    case Look::Hidden:
        return i18n::t("Show the preview again");
    case Look::Embed: {
        // The page's text in full up to 300 characters, cut between whole characters with "…" (never half an emoji).
        QStringList lines;
        lines << cleanText(card.data.title, kEmbedTipChars);
        if (!card.data.description.isEmpty())
            lines << cleanText(card.data.description, kEmbedTipChars);
        lines << (card.data.play ? i18n::t("Plays on %1 in your browser").arg(card.host) : i18n::t("Opens %1 in your browser").arg(card.host));
        if (!card.redirectedTo.isEmpty())
            lines << i18n::t("This preview comes from %1, where the link redirected when it was loaded").arg(card.redirectedTo);
        if (card.fromSender)
            lines << i18n::t("Preview made by the sender's TS Media");
        if (covered(card))
            lines << i18n::t("Marked as sensitive. Click the picture to show it.");
        return lines.join(QLatin1Char('\n'));
    }
    case Look::Absent:
        return {};
    }
    return {};
}

QString toolTipHtml(const Card& card, Zone zone)
{
    const QString plain = toolTip(card, zone);
    if (plain.isEmpty())
        return {};
    if (card.look == Look::YouTube && card.player == Player::Consent && zone == Zone::Body)
        return QString::fromLatin1("<p>") + plain.toHtmlEscaped() + QString::fromLatin1("</p>"); // the notice wraps
    // 2.5: an Embed card's own tip starts with the page's title and text (up to 300 characters each): those lines wrap
    // (QToolTip wraps rich text at about 80 characters), so a long text never makes a tip wider than the screen; the
    // lines after them (hosts, notes) never break.
    const bool embedTip  = card.look == Look::Embed && zone != Zone::Hide && zone != Zone::Reveal && zone != Zone::None;
    const int  pageLines = embedTip ? (card.data.description.isEmpty() ? 1 : 2) : 0;
    QString    html;
    int        index = 0;
    for (const QString& line : plain.split(QLatin1Char('\n'))) {
        if (!html.isEmpty())
            html += QString::fromLatin1("<br>");
        if (!embedTip || index < pageLines)
            html += line.toHtmlEscaped();
        else
            html += QString::fromLatin1("<nobr>") + line.toHtmlEscaped() + QString::fromLatin1("</nobr>");
        ++index;
    }
    if (embedTip)
        return QString::fromLatin1("<p>") + html + QString::fromLatin1("</p>");
    return QString::fromLatin1("<p style='white-space:pre'>") + html + QString::fromLatin1("</p>");
}

// ---- geometry -----------------------------------------------------------------------------------------------

Layout layout(const Card& card, const PreviewStyle& style)
{
    Layout      l;
    const qreal pad = padOf(style);
    if (card.look == Look::Absent)
        return l; // nothing at all (size 0)
    if (card.look == Look::Embed)
        return embedLayout(card, style);
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
    if (card.look == Look::YouTube && card.player == Player::Consent)
        consentLayout(card, style, l); // 2.5 YouTube player: inside the same size
    return l;
}

bool playerOpen(const Card& card)
{
    return card.look == Look::YouTube
           && (card.player == Player::Starting || card.player == Player::Open || card.player == Player::Popup || card.player == Player::Hidden);
}

Zone primaryZone(const Card& card)
{
    switch (card.look) {
    case Look::Click:
        return Zone::Load;
    case Look::Error:
        return card.nothing ? Zone::Open : Zone::Retry;
    case Look::TooLarge:
    case Look::YouTube:
    case Look::Poster:
    case Look::Embed:
        return Zone::Open;
    case Look::Hidden:
        return Zone::Show;
    case Look::Loading:
    case Look::Absent:
        return Zone::Body;
    }
    return Zone::Body;
}

QString signature(const Card& card, const PreviewStyle& style, const Pointer& pointer)
{
    // Everything render() draws from: a card is drawn again only when one of these changes. Text is in as shown
    // (statusText: a "Site busy" countdown changes the signature when its number changes, not every millisecond).
    const linkpreview::CardData& d = card.data;
    QStringList                  parts;
    parts << QString::number(style.dark) << style.font.toString() << QString::number(style.dpr) << QString::number(style.maxWidth)
          << (style.bubbles.isEmpty() ? QString() : style.bubbles.first().name(QColor::HexArgb)) << QString::number(style.animate)
          << QString::number(static_cast<int>(card.look)) << QString::number(static_cast<int>(card.kind)) << card.host << card.title << card.author
          << QString::number(card.progress, 'f', 2) << QString::number(static_cast<int>(card.waiting)) << card.error << QString::number(card.size)
          << QString::number(card.overAutoLimit) << QString::number(card.autoLimit) << QString::number(card.durationMs)
          << QString::number(card.thumbnail.isNull() ? 0 : card.thumbnail.cacheKey()) << QString::number(card.thumbnailLoading)
          // 2.5
          << QString::number(static_cast<int>(card.cls)) << card.provider << QString::number(card.slotIndex) << card.redirectedTo
          << QString::number(card.fromSender) << QString::number(card.nothing) << card.busySite << statusText(card)
          << QString::number(card.picture.isNull() ? 0 : card.picture.cacheKey()) << QString::number(card.pictureLoading)
          << QString::number(card.revealed) << d.site << d.author << d.title << d.description << QString::number(d.pictureWidth)
          << QString::number(d.pictureHeight) << QString::number(static_cast<int>(d.layout)) << QString::number(d.hasColor)
          << QString::number(d.color) << QString::number(d.play) << QString::number(d.durationMs) << QString::number(d.sensitive)
          << QString::number(pointer.over) << QString::number(static_cast<int>(pointer.hover)) << QString::number(static_cast<int>(pointer.pressed))
          // 2.5 YouTube player
          << QString::number(static_cast<int>(card.player)) << QString::number(card.playable) << card.note << QString::number(card.resumeMs / 1000);
    return parts.join(QLatin1Char('|'));
}

Zone menuDefault(const Card& card)
{
    const Zone zone = primaryZone(card);
    return zone == Zone::Body ? Zone::Open : zone;
}

bool isActionable(const Card& card, Zone zone)
{
    if (zone == Zone::Hide || zone == Zone::Show || zone == Zone::Load || zone == Zone::Retry || zone == Zone::Open || zone == Zone::Reveal)
        return true;
    if (zone == Zone::Play || zone == Zone::Stop || zone == Zone::ConsentPlay || zone == Zone::ConsentBrowser || zone == Zone::Terms || zone == Zone::Privacy)
        return true;
    // 2.5 YouTube player: the Consent state's text, a starting player and the covered thumbnail do nothing.
    if (card.look == Look::YouTube && (card.player == Player::Consent || card.player == Player::Starting || card.player == Player::Open))
        return false;
    return zone == Zone::Body && card.look != Look::Loading && card.look != Look::Absent;
}

Zone zoneAt(const Card& card, const Layout& l, const QPointF& pos, bool over)
{
    if (card.look == Look::Absent || !QRectF(QPointF(0, 0), QSizeF(l.size)).contains(pos))
        return Zone::None;
    if (card.look == Look::Hidden)
        return Zone::Show;
    if (card.look == Look::YouTube) {
        // 2.5 YouTube player: the Consent state answers with its pills and links only; an open player's (x) is Stop.
        if (card.player == Player::Consent) {
            for (const auto& [rect, zone] : {std::make_pair(l.consentPlay, Zone::ConsentPlay), std::make_pair(l.consentBrowser, Zone::ConsentBrowser),
                                             std::make_pair(l.terms, Zone::Terms), std::make_pair(l.privacy, Zone::Privacy)}) {
                if (!rect.isEmpty() && rect.adjusted(-2, -3, 2, 3).contains(pos))
                    return zone;
            }
            return Zone::Body;
        }
        if (playerOpen(card) && l.hide.adjusted(-2, -2, 2, 2).contains(pos))
            return Zone::Stop;
    }
    if (over && l.hide.adjusted(-2, -2, 2, 2).contains(pos))
        return Zone::Hide;
    if (card.look == Look::YouTube && card.player != Player::Note && l.thumb.contains(pos)) {
        if (card.player == Player::Starting || card.player == Player::Open)
            return Zone::Body; // starting, or covered by the player
        if (card.playable && (card.player == Player::None || card.player == Player::Hidden || card.player == Player::Popup))
            return Zone::Play;
    }
    if (card.look == Look::Embed && covered(card) && l.thumb.contains(pos))
        return Zone::Reveal;
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
    if (card.look == Look::Absent || l.size.isEmpty())
        return {};
    QImage   out = canvas(l.size, ratio(style));
    QPainter p(&out);
    prepare(p);
    p.setLayoutDirection(Qt::LeftToRight);
    const qreal w        = l.size.width();
    const qreal h        = l.size.height();
    const bool  hovered  = pointer.over && isActionable(card, pointer.hover == Zone::None ? Zone::Body : pointer.hover);
    const bool  pressed  = hovered && pointer.pressed != Zone::None && pointer.pressed == pointer.hover;
    const QColor tint    = readableTint(card, style, pal);
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

    if (card.look == Look::Embed) {
        renderEmbed(p, card, l, pal, style, pointer, hovered && pointer.hover != Zone::Reveal, pressed);
    } else if (!rich(card)) {
        // The glyph on its tinted square, or the ring / the alert.
        QColor square = tint;
        square.setAlpha(style.dark ? 46 : 30);
        p.setPen(Qt::NoPen);
        p.setBrush(card.look == Look::Error ? QColor(pal.errorFill.red(), pal.errorFill.green(), pal.errorFill.blue(), style.dark ? 46 : 30) : square);
        p.drawRoundedRect(l.icon, 8, 8);
        const QRectF glyph = l.icon.adjusted(6, 6, -6, -6);
        // "Site busy" for more than a minute is still: the glyph, no ring (nothing spins for minutes).
        if (card.look == Look::Loading && !coolingStill(card))
            drawRing(p, l.icon.center(), 9, 2.2, card.waiting == Waiting::Host ? -1.0 : card.progress, pal.track, pal.progress, style.animate);
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
        p.drawText(QRectF(l.text.left(), top, l.text.width(), tfm.height()), align, elidedHost(tfm, card.host, l.text.width()));
        p.setFont(subFont);
        p.setPen(card.look == Look::Error && !card.nothing ? pal.errorText : pal.muted);
        // From most to least complete; a narrow chat gets a short line that still says it all.
        QStringList status{statusText(card)};
        if (card.look == Look::TooLarge) {
            if (card.size > 0)
                status << i18n::t("Too large · %1").arg(formatSize(static_cast<quint64>(card.size)));
            status << i18n::t("Too large");
        }
        if (card.look == Look::Error) {
            if (card.nothing) {
                status << i18n::t("No preview");
            } else {
                if (!card.error.isEmpty())
                    status << i18n::t("Couldn't load the preview");
                status << i18n::t("Couldn't load");
            }
        }
        if (card.look == Look::Loading) {
            // The short forms keep what it waits for (a narrow chat): "Loading…" alone would look like a website load.
            if (card.waiting == Waiting::Own) {
                status << i18n::t("Making preview…");
            } else if (card.waiting == Waiting::Sender) {
                status << i18n::t("Waiting for the sender…") << i18n::t("Sender's preview…");
            } else if (card.waiting == Waiting::Checking) {
                status << i18n::t("Checking for a preview…") << i18n::t("Checking…");
            } else if (card.waiting == Waiting::Host) {
                if (coolingStill(card))
                    status << i18n::t("Busy · %1 min").arg(minutesText(card.retryInMs)) << i18n::t("Busy");
                else
                    status << i18n::t("Busy · %1 s").arg(secondsText(card.retryInMs)) << i18n::t("Busy…");
            }
            if (card.waiting != Waiting::Host)
                status << (card.progress >= 0 && card.waiting == Waiting::None ? i18n::t("Loading… %1%").arg(qRound(qBound(0.0, card.progress, 1.0) * 100))
                                                                              : i18n::t("Loading…"));
        }
        if (card.look == Look::Click && (card.size > 0 || (card.overAutoLimit && card.autoLimit > 0)))
            status << stripLabel(card);
        p.drawText(QRectF(l.text.left(), top + tfm.height() + 2, l.text.width(), sfm.height()), align, fitText(sfm, status, l.text.width()));

        // The action on the right: its icon, and its label where there is room.
        if (!l.action.isEmpty()) {
            const bool   on   = pointer.over && (pointer.hover == primaryZone(card) || pointer.hover == Zone::Body);
            const QColor ink  = pressed ? pal.progress : on ? pal.link : pal.muted;
            const QRectF icon(l.action.left(), l.action.center().y() - 9, 18, 18);
            if (card.look == Look::Click)
                drawDownloadIcon(p, icon, ink);
            else if (card.look == Look::Error && !card.nothing)
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
        const QColor bar     = tint;
        // 2.5 YouTube player: under the Consent state only the bar and the picture (its text sits on a quiet background).
        const bool   consent = card.look == Look::YouTube && card.player == Player::Consent;
        if (card.look == Look::YouTube) {
            p.setPen(Qt::NoPen);
            p.setBrush(bar);
            const qreal x = padOf(style);
            p.drawRoundedRect(QRectF(x, onBubble(style) ? 4 : kPad, kBar, h - (onBubble(style) ? 8 : 2 * kPad)), kBar / 2.0, kBar / 2.0);
        }
        if (card.look == Look::YouTube && !consent) {
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
            p.setPen(hovered && pointer.hover != Zone::Play ? pal.link : pal.title); // the thumbnail's Play: the title stays
            for (int i = 0; i < lines.size(); ++i)
                p.drawText(QRectF(l.title.left(), l.title.top() + i * bfm.lineSpacing(), cw, bfm.lineSpacing()), align, lines.at(i));
        }
        drawThumb(p, card, l, pal, style, hovered, pressed);
    }
    if (rich(card) && !(card.look == Look::YouTube && card.player == Player::Consent)) {
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

    // 2.5 YouTube player: the Consent state over everything (no hide button on it).
    if (card.look == Look::YouTube && card.player == Player::Consent) {
        drawConsent(p, l, style, pointer);
        return out;
    }
    // An open player's (x) is always there: Stop.
    if (playerOpen(card)) {
        const bool on = pointer.over && pointer.hover == Zone::Stop;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.placeholderHover);
            p.drawEllipse(l.hide);
        }
        drawCloseGlyph(p, l.hide, on ? pal.title : pal.muted);
        return out;
    }
    // The hide button, while the pointer is on the card.
    if (pointer.over && card.look != Look::Hidden) {
        const bool   on       = pointer.hover == Zone::Hide;
        const QRectF r        = l.hide;
        const bool   onPicture = card.look == Look::Poster || (card.look == Look::Embed && l.thumbSide);
        if (on || onPicture) {
            p.setPen(Qt::NoPen);
            p.setBrush(onPicture ? QColor(0, 0, 0, on ? 200 : 150) : pal.placeholderHover);
            p.drawEllipse(r);
        }
        drawCloseGlyph(p, r, onPicture ? QColor(Qt::white) : on ? pal.title : pal.muted);
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
    // 2.5 Embed cards (9.3)
    out.append({QStringLiteral("link card glyph Web page"), tintOf(linkpreview::Kind::Card, dark), pal.background, 3.0});
    out.append({QStringLiteral("embed site line"), pal.muted, pal.background, 4.5});
    out.append({QStringLiteral("embed author"), pal.title, pal.background, 4.5});
    out.append({QStringLiteral("embed description"), pal.title, pal.background, 4.5});
    out.append({QStringLiteral("embed title (hover)"), pal.link, pal.backgroundHover, 4.5});
    out.append({QStringLiteral("embed footer host"), pal.muted, pal.background, 4.5});
    // The duration pill: white on black at 184/255 over a white picture.
    const int alpha = 184;
    const QColor pill(255 - (255 * alpha) / 255, 255 - (255 * alpha) / 255, 255 - (255 * alpha) / 255);
    out.append({QStringLiteral("embed duration"), QColor(Qt::white), pill, 4.5});
    PreviewStyle style;
    style.dark = dark;
    for (const QString& id : providers::ids()) {
        const providers::Brand brand = providers::brandOf(id);
        if (!brand.name)
            continue;
        Card card;
        card.cls      = linkpreview::LinkClass::Site;
        card.provider = id;
        card.kind     = linkpreview::Kind::Card;
        out.append({QStringLiteral("embed brand bar ") + id, barColour(card, style), pal.background, 3.0});
        out.append({QStringLiteral("embed brand line ") + id, brandLineColour(card, style), pal.background, 4.5});
    }
    return out;
}

} // namespace linkcard
