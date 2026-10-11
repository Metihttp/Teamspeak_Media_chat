#include "layoutart.h"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <QVector>

#include <cmath>

#include "emojidata.h"
#include "emojirender.h"
#include "emojitext.h"
#include "i18n.h"
#include "uiutil.h"

namespace layoutart {

namespace {

int even(qreal x)
{
    return 2 * qRound(x / 2.0);
}

int scaled(int value, int f)
{
    return qMax(0, qRound(value * f / 12.0));
}

QFont sized(const QFont& base, qreal px, int weight = -1)
{
    QFont font(base);
    font.setPixelSize(qMax(1, qRound(px)));
    if (weight >= 0)
        font.setWeight(weight);
    font.setUnderline(false);
    font.setItalic(false);
    font.setStrikeOut(false);
    return font;
}

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}

QColor alpha(const QColor& c, qreal a)
{
    QColor out = c;
    out.setAlphaF(a);
    return out;
}

double worst(const QColor& c, const QVector<QColor>& backs)
{
    double low = 99;
    for (const QColor& b : backs)
        low = qMin(low, ui::contrastRatio(c, b));
    return low;
}

// c moved towards white (dark backgrounds) or black in small steps until it keeps minimum:1 on every back.
QColor nudge(const QColor& color, const QVector<QColor>& backs, double minimum)
{
    QColor c = color;
    c.setAlpha(255);
    if (backs.isEmpty())
        return c;
    const QColor target = backs.first().lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    for (int step = 0; step < 60 && worst(c, backs) < minimum; ++step)
        c = mix(c, target, 0.03);
    return c;
}

bool near(const QColor& a, const QColor& b)
{
    return qAbs(a.red() - b.red()) <= 3 && qAbs(a.green() - b.green()) <= 3 && qAbs(a.blue() - b.blue()) <= 3;
}

QPointF at(const QRectF& box, qreal x, qreal y)
{
    return QPointF(box.left() + x * box.width(), box.top() + y * box.height());
}

QImage canvas(const QSize& size, qreal dpr)
{
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QImage      img(qMax(1, qRound(size.width() * ratio)), qMax(1, qRound(size.height() * ratio)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(ratio);
    img.fill(Qt::transparent);
    return img;
}

void prepare(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setLayoutDirection(Qt::LeftToRight);
}

QString firstGrapheme(const QString& text)
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return QString(QLatin1Char('?'));
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, t);
    const int           end = finder.toNextBoundary();
    return end > 0 ? t.left(end) : t.left(1);
}

} // namespace

// ============================================================================================
// Tokens
// ============================================================================================

Tokens tokensFor(Mode mode, const QFont& chatFont, bool ampm, bool bubbles)
{
    Tokens t;
    t.mode = mode;
    t.ampm = ampm;
    int f  = QFontInfo(chatFont).pixelSize();
    if (f <= 0)
        f = 12;
    f       = qBound(8, f, 40);
    t.f     = f;
    t.body  = chatFont;
    t.name  = sized(chatFont, f, QFont::DemiBold);
    t.time  = sized(chatFont, qMax(10.0, 0.84 * f));
    t.compactTime = sized(chatFont, 0.92 * f);
    t.divider     = sized(chatFont, 0.88 * f, QFont::DemiBold);
    t.reply       = sized(chatFont, qMax(10.0, 0.92 * f));
    t.chip        = sized(chatFont, qMax(10.0, 0.84 * f));

    t.G            = even(4.0 * f);
    t.avatar       = even(2.0 * f);
    t.avatarX      = scaled(8, f);
    t.headRow      = t.avatar;
    t.replyRow     = even(1.5 * f);
    t.lineMin      = qRound(1.42 * f);
    t.headTop      = scaled(14, f);
    t.afterDivider = scaled(6, f);
    t.systemTop    = scaled(8, f);
    t.systemRunTop = scaled(2, f);
    t.nameGap      = scaled(8, f);
    if (mode == Mode::Compact) {
        t.dividerTop    = scaled(10, f);
        t.dividerHeight = even(1.5 * f);
        t.dividerBottom = scaled(2, f);
    } else {
        t.dividerTop    = scaled(18, f);
        t.dividerHeight = even(1.66 * f);
        t.dividerBottom = scaled(2, f);
    }
    const QFontMetricsF tm(t.compactTime);
    t.L                = static_cast<int>(std::ceil(tm.horizontalAdvance(ampm ? QStringLiteral("12:59 PM") : QStringLiteral("00:00")))) + 10;
    t.compactLeading   = 2;
    t.compactHeadTop   = scaled(4, f);
    t.compactSystemTop = scaled(2, f);
    t.compactNameGap   = scaled(7, f);

    // 2.3 chat-visuals: bubbles (Cozy only) and centred system rows.
    t.bubbles            = bubbles && mode == Mode::Cozy;
    t.bubblePadX         = scaled(10, f);
    t.bubblePadTop       = scaled(4, f);
    t.bubblePadHead      = 0;
    t.bubblePadBottom    = scaled(5, f);
    t.bubbleGap          = qMax(2, scaled(3, f));
    t.bubbleHeadTop      = scaled(16, f);
    t.bubbleRight        = scaled(18, f);
    t.bubbleRadius       = scaled(12, f);
    t.bubbleRadiusJoined = qMax(3, scaled(5, f));
    t.bubbleMinWidth     = scaled(44, f);
    t.pillPadX           = scaled(10, f);
    t.pillPadY           = qMax(1, scaled(2, f));
    t.pillInset          = mode == Mode::Compact ? scaled(16, f) : scaled(24, f);
    t.systemPrefixWidth  = scaled(22, f);
    if (t.bubbles) {
        t.G += even(f / 3.0); // the bubble's padding goes left of the text: the avatar keeps its air
        t.systemTop    = scaled(12, f);
        t.systemRunTop = scaled(8, f);
    }
    // 2.3.1: own messages on the right and the chat's room at its ends.
    const QFontMetricsF om(t.time);
    t.ownTimeGap = qMax(6, scaled(8, f));
    t.ownTimeRow = static_cast<int>(std::ceil(om.ascent() + om.descent()));
    t.ownSlack   = 2;
    t.chatTop    = scaled(4, f);
    t.chatBottom = mode == Mode::Compact ? scaled(12, f) : scaled(14, f);
    // 2.3.1: a body line is ascent + descent high (QTextLine::height); Qt puts one lower than lineMin at
    // the bottom of its line, lift lower.
    const QFontMetricsF bm(chatFont);
    const int           lift = qMax(0, t.lineMin - static_cast<int>(std::ceil(bm.ascent() + bm.descent() - 0.01)));
    t.ownHeadTop             = qMax(0, t.bubblePadTop - t.bubblePadHead - lift);
    return t;
}

qreal bodyAscent(const Tokens& tokens)
{
    return QFontMetricsF(tokens.body).ascent();
}

// ============================================================================================
// Colours
// ============================================================================================

Colors colorsFor(bool dark, const QColor& baseIn, const QColor& skinText, const QColor& skinLink)
{
    Colors c;
    c.dark = dark;
    const QColor tsDark(0x2f, 0x31, 0x36);
    const QColor white(0xff, 0xff, 0xff);
    QColor       base = baseIn.isValid() && baseIn.alpha() == 255 ? baseIn : (dark ? tsDark : white);
    if ((base.lightness() < 128) != dark)
        base = dark ? tsDark : white;
    c.base = base;
    c.text = skinText.isValid() && ui::contrastRatio(skinText, base) >= 4.5 ? skinText : (dark ? QColor(0xdc, 0xdd, 0xde) : QColor(0, 0, 0));
    c.text.setAlpha(255);

    c.hover       = dark ? QColor(0, 0, 0, 36) : QColor(6, 6, 7, 10);
    c.mentionTint = dark ? QColor(240, 178, 50, 13) : QColor(250, 168, 26, 31);
    c.pillBack    = dark ? QColor(88, 101, 242, 77) : QColor(88, 101, 242, 38);
    const QColor hover   = ui::flatten(c.hover, base);
    const QColor mention = ui::flatten(c.mentionTint, base);

    const bool knownDark  = near(base, tsDark);
    const bool knownLight = near(base, white);
    if (knownDark) {
        c.name  = QColor(0xf2, 0xf3, 0xf5);
        c.muted = QColor(0x9a, 0x9f, 0xa8);
    } else if (knownLight) {
        c.name  = QColor(0x06, 0x06, 0x07);
        c.muted = QColor(0x5c, 0x5f, 0x66);
    } else {
        c.name  = nudge(mix(c.text, dark ? white : QColor(0, 0, 0), 0.5), {base}, 7.0);
        c.muted = nudge(mix(c.text, base, 0.45), {base, hover, mention}, 4.5);
    }
    // Links: the skin's colour when it reads (base and hover row), else nudged until it does.
    QColor link = skinLink.isValid() ? skinLink : (dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
    link.setAlpha(255);
    if (worst(link, {base, hover, mention}) < 4.5)
        link = (knownLight && near(link, QColor(0x1c, 0x82, 0xcc))) ? QColor(0x18, 0x72, 0xb8) : nudge(link, {base, hover, mention}, 4.5);
    c.link          = link;
    c.linkUnderline = alpha(link, 0.4);

    c.mentionBar = dark ? QColor(0xf0, 0xb2, 0x32) : QColor(0xb8, 0x83, 0x00);
    c.pillText   = dark ? QColor(0xd7, 0xda, 0xfd) : QColor(0x3a, 0x44, 0xb5);
    c.divider    = knownDark ? QColor(0x45, 0x48, 0x4f) : knownLight ? QColor(0xe1, 0xe3, 0xe6) : mix(base, c.text, 0.16);
    c.focusBar   = dark ? QColor(0x79, 0x83, 0xf5) : QColor(0x58, 0x65, 0xf2);
    c.accent     = dark ? QColor(0x79, 0x83, 0xf5) : QColor(0x58, 0x65, 0xf2);
    c.join       = nudge(dark ? QColor(0x3b, 0xa5, 0x5c) : QColor(0x24, 0x80, 0x46), {base}, 3.0);
    c.leave      = nudge(dark ? QColor(0xf2, 0x7a, 0x7d) : QColor(0xc9, 0x37, 0x3d), {base}, 3.0);
    c.announce   = nudge(dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x58, 0x65, 0xf2), {base}, 3.0);
    c.danger     = nudge(dark ? QColor(0xf2, 0x7a, 0x7d) : QColor(0xc9, 0x37, 0x3d), {base, hover, mention}, 4.5);
    c.spine      = dark ? QColor(0x5c, 0x60, 0x68) : QColor(0xc4, 0xc9, 0xce);
    c.chipBorder = alpha(c.muted, 0.4);
    c.mutedOnMention = nudge(c.muted, {mention}, 4.5);
    c.chipHover  = alpha(c.text, 0.08);

    if (dark) {
        c.barBack      = knownDark ? QColor(0x36, 0x39, 0x3f) : ui::flatten(QColor(255, 255, 255, 12), base);
        c.barBorder    = QColor(0x1f, 0x20, 0x23);
        c.barIcon      = QColor(0xb5, 0xba, 0xc1);
        c.barHoverBack = QColor(0x45, 0x48, 0x4f);
        c.barHoverIcon = QColor(0xf2, 0xf3, 0xf5);
        c.barPressed   = QColor(0x4e, 0x50, 0x58);
    } else {
        c.barBack      = QColor(0xff, 0xff, 0xff);
        c.barBorder    = QColor(0xdd, 0xe0, 0xe4);
        c.barIcon      = QColor(0x4e, 0x50, 0x58);
        c.barHoverBack = QColor(0xeb, 0xed, 0xef);
        c.barHoverIcon = QColor(0x06, 0x06, 0x07);
        c.barPressed   = QColor(0xe3, 0xe5, 0xe8);
    }
    c.barShadow = QColor(0, 0, 0, 61);

    // 2.3 chat-visuals: bubbles a step lighter than a dark chat (a raised surface) and a step darker than a
    // light one, yours tinted with the accent; translucent, so the same colours work over the text too.
    if (dark) {
        c.bubble      = QColor(255, 255, 255, 13);
        c.bubbleOwn   = alpha(c.accent, 0.16);
        c.bubbleHover = QColor(255, 255, 255, 9);
        c.bubbleEdge  = QColor(255, 255, 255, 0);
        c.pill        = QColor(255, 255, 255, 9);
    } else {
        c.bubble      = QColor(6, 10, 22, 14);
        c.bubbleOwn   = alpha(c.accent, 0.12);
        c.bubbleHover = QColor(6, 10, 22, 9);
        c.bubbleEdge  = QColor(6, 10, 22, 16);
        c.pill        = QColor(6, 10, 22, 10);
    }
    // TeamSpeak's body text isn't ours to recolour: on a skin where it barely reads, the bubbles get fainter.
    for (int step = 0; step < 4; ++step) {
        double low = 99;
        for (const bool own : {false, true})
            low = qMin(low, ui::contrastRatio(c.text, bubbleBack(c, own, true, true)));
        if (low >= 4.5)
            break;
        for (QColor* tint : {&c.bubble, &c.bubbleOwn, &c.bubbleHover, &c.pill})
            tint->setAlpha(tint->alpha() / 2);
    }
    QVector<QColor> bubbles;
    for (const bool own : {false, true}) {
        for (const bool hovered : {false, true}) {
            for (const bool mentioned : {false, true})
                bubbles.append(bubbleBack(c, own, hovered, mentioned));
        }
    }
    const QVector<QColor> pills = {pillBack(c, false), pillBack(c, true)};
    c.mutedOnBubble = nudge(c.muted, bubbles + pills, 4.5);
    c.linkOnBubble  = nudge(c.link, bubbles, 4.5);
    c.dangerOnPill  = nudge(c.danger, pills, 4.5);
    // A mention's pill on a bubble too.
    QVector<QColor> mentionPills = {ui::flatten(c.pillBack, base), ui::flatten(c.pillBack, mention)};
    for (const QColor& back : qAsConst(bubbles))
        mentionPills.append(ui::flatten(c.pillBack, back));
    c.pillText = nudge(c.pillText, mentionPills, 4.5);
    // The system icons on their pills too (a no-op where they read already).
    c.join     = nudge(c.join, QVector<QColor>{base} + pills, 3.0);
    c.leave    = nudge(c.leave, QVector<QColor>{base} + pills, 3.0);
    c.announce = nudge(c.announce, QVector<QColor>{base} + pills, 3.0);
    return c;
}

QColor bubbleBack(const Colors& colors, bool own, bool hover, bool mention)
{
    QColor back = ui::flatten(own ? colors.bubbleOwn : colors.bubble, colors.base);
    if (mention)
        back = ui::flatten(colors.mentionTint, back);
    if (hover)
        back = ui::flatten(colors.bubbleHover, back);
    return back;
}

QColor pillBack(const Colors& colors, bool hover)
{
    const QColor back = ui::flatten(colors.pill, colors.base);
    return hover ? ui::flatten(colors.bubbleHover, back) : back;
}

QVector<QColor> bubbleSurfaces(const Colors& colors)
{
    QVector<QColor> out;
    for (const bool own : {false, true}) {
        for (const bool hovered : {false, true}) {
            for (const bool mentioned : {false, true})
                out.append(bubbleBack(colors, own, hovered, mentioned));
        }
    }
    return out;
}

QColor hoverRow(const Colors& colors)
{
    return ui::flatten(colors.hover, colors.base);
}

QColor mentionRow(const Colors& colors)
{
    return ui::flatten(colors.mentionTint, colors.base);
}

QColor nameColorFor(const Colors& colors, const QColor& nickColor, const QColor& usualNickColor)
{
    if (!nickColor.isValid() || !usualNickColor.isValid() || near(nickColor, usualNickColor))
        return colors.name;
    // 2.3 chat-visuals: and on the bubbles (a head's name is drawn on one)
    return nudge(nickColor, {colors.base, hoverRow(colors), bubbleBack(colors, false), bubbleBack(colors, true), bubbleBack(colors, false, true), bubbleBack(colors, true, true)}, 4.5);
}

QVector<Pair> contrastPairs(const Colors& c)
{
    const QColor  hover   = hoverRow(c);
    const QColor  mention = mentionRow(c);
    QVector<Pair> pairs;
    const auto text = [&](const QString& what, const QColor& fore) {
        pairs.append({what + QStringLiteral(" on base"), fore, c.base, 4.5});
        pairs.append({what + QStringLiteral(" on hover"), fore, hover, 4.5});
        pairs.append({what + QStringLiteral(" on mention"), fore, mention, 4.5});
    };
    text(QStringLiteral("body"), c.text);
    text(QStringLiteral("name"), c.name);
    pairs.append({QStringLiteral("muted on base"), c.muted, c.base, 4.5});
    pairs.append({QStringLiteral("muted on hover"), c.muted, hover, 4.5});
    pairs.append({QStringLiteral("muted on mention"), c.mutedOnMention, mention, 4.5});
    text(QStringLiteral("link"), c.link);
    text(QStringLiteral("danger"), c.danger);
    pairs.append({QStringLiteral("pill text"), c.pillText, ui::flatten(c.pillBack, c.base), 4.5});
    pairs.append({QStringLiteral("pill text on mention"), c.pillText, ui::flatten(c.pillBack, mention), 4.5});
    pairs.append({QStringLiteral("join icon"), c.join, c.base, 3.0});
    pairs.append({QStringLiteral("leave icon"), c.leave, c.base, 3.0});
    pairs.append({QStringLiteral("announce icon"), c.announce, c.base, 3.0});
    pairs.append({QStringLiteral("muted icon"), c.muted, c.base, 3.0});
    pairs.append({QStringLiteral("focus bar"), c.focusBar, c.base, 3.0});
    pairs.append({QStringLiteral("bar icon"), c.barIcon, c.barBack, 3.0});
    pairs.append({QStringLiteral("bar icon hovered"), c.barHoverIcon, c.barHoverBack, 3.0});
    pairs.append({QStringLiteral("bar icon pressed"), c.barHoverIcon, c.barPressed, 3.0});
    pairs.append({QStringLiteral("initials"), QColor(Qt::white), initialsColor(QStringLiteral("a")), 4.5});
    // 2.3 chat-visuals: everything drawn on a bubble or a pill, in every state
    for (const bool own : {false, true}) {
        for (const bool hovered : {false, true}) {
            for (const bool mentioned : {false, true}) {
                const QColor  back = bubbleBack(c, own, hovered, mentioned);
                const QString what = QStringLiteral(" on %1%2%3bubble").arg(own ? QStringLiteral("own ") : QString(), hovered ? QStringLiteral("hovered ") : QString(),
                                                                           mentioned ? QStringLiteral("mention ") : QString());
                pairs.append({QStringLiteral("body") + what, c.text, back, 4.5});
                pairs.append({QStringLiteral("name") + what, c.name, back, 4.5});
                pairs.append({QStringLiteral("muted") + what, c.mutedOnBubble, back, 4.5});
                pairs.append({QStringLiteral("link") + what, c.linkOnBubble, back, 4.5});
                pairs.append({QStringLiteral("pill text") + what, c.pillText, ui::flatten(c.pillBack, back), 4.5});
            }
        }
    }
    for (const bool hovered : {false, true}) {
        const QColor  back = pillBack(c, hovered);
        const QString what = hovered ? QStringLiteral(" on hovered pill") : QStringLiteral(" on pill");
        pairs.append({QStringLiteral("muted") + what, c.mutedOnBubble, back, 4.5});
        pairs.append({QStringLiteral("name") + what, c.name, back, 4.5});
        pairs.append({QStringLiteral("danger") + what, c.dangerOnPill, back, 4.5});
        pairs.append({QStringLiteral("icon") + what, c.join, back, 3.0});
        pairs.append({QStringLiteral("leave icon") + what, c.leave, back, 3.0});
        pairs.append({QStringLiteral("announce icon") + what, c.announce, back, 3.0});
    }
    return pairs;
}

// ============================================================================================
// Labels
// ============================================================================================

namespace {

struct Clock {
    int  h = -1, m = 0, s = 0;
    bool ampm = false;
    bool pm   = false;
};

Clock clockOf(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{1,2}):(\\d{2})(?::(\\d{2}))?\\s*([AaPp][Mm])?\\s*$"));
    const QRegularExpressionMatch   match = re.match(text);
    Clock                           c;
    if (!match.hasMatch())
        return c;
    int       h = match.captured(1).toInt();
    const int m = match.captured(2).toInt();
    const int s = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt();
    if (m > 59 || s > 59)
        return c;
    if (!match.captured(4).isEmpty()) {
        if (h < 1 || h > 12)
            return c;
        c.ampm = true;
        c.pm   = match.captured(4).startsWith(QLatin1Char('p'), Qt::CaseInsensitive);
        h      = h % 12 + (c.pm ? 12 : 0);
    } else if (h > 23) {
        return c;
    }
    c.h = h;
    c.m = m;
    c.s = s;
    return c;
}

QString clockText(const Clock& c, bool ampm, bool seconds)
{
    if (c.h < 0)
        return {};
    if (ampm) {
        const int h12 = c.h % 12 == 0 ? 12 : c.h % 12;
        return seconds ? QStringLiteral("%1:%2:%3 %4").arg(h12).arg(c.m, 2, 10, QLatin1Char('0')).arg(c.s, 2, 10, QLatin1Char('0')).arg(c.h >= 12 ? QStringLiteral("PM") : QStringLiteral("AM"))
                       : QStringLiteral("%1:%2 %3").arg(h12).arg(c.m, 2, 10, QLatin1Char('0')).arg(c.h >= 12 ? QStringLiteral("PM") : QStringLiteral("AM"));
    }
    return seconds ? QStringLiteral("%1:%2:%3").arg(c.h, 2, 10, QLatin1Char('0')).arg(c.m, 2, 10, QLatin1Char('0')).arg(c.s, 2, 10, QLatin1Char('0'))
                   : QStringLiteral("%1:%2").arg(c.h, 2, 10, QLatin1Char('0')).arg(c.m, 2, 10, QLatin1Char('0'));
}

} // namespace

QString shortTime(const QString& headerTime, bool ampm)
{
    return clockText(clockOf(headerTime), ampm, false);
}

int secondsOf(const QString& headerTime)
{
    const Clock c = clockOf(headerTime);
    return c.h < 0 ? -1 : c.h * 3600 + c.m * 60 + c.s;
}

bool hasAmPm(const QString& headerTime)
{
    return clockOf(headerTime).ampm;
}

QString headTimeLabel(const QDate& date, const QDate& today, const QString& headerTime, bool ampm)
{
    const QString time = shortTime(headerTime, ampm);
    if (time.isEmpty())
        return {};
    if (!date.isValid())
        return time;
    if (date == today)
        return i18n::t("Today at %1").arg(time);
    if (date == today.addDays(-1))
        return i18n::t("Yesterday at %1").arg(time);
    return QLocale::system().toString(date, QLocale::ShortFormat) + QLatin1Char(' ') + time;
}

QString fullTimeLabel(const QDate& date, const QString& headerTime)
{
    const Clock   c    = clockOf(headerTime);
    const QString time = clockText(c, c.ampm, true);
    if (!date.isValid())
        return time;
    const QString day = QLocale(QLocale::English).toString(date, QStringLiteral("dddd, MMMM d, yyyy"));
    return time.isEmpty() ? day : day + QLatin1Char(' ') + time;
}

QString dayLabel(const QDate& date, const QDate& today)
{
    const QLocale english(QLocale::English);
    if (date == today)
        return i18n::t("Today, %1").arg(english.toString(date, QStringLiteral("MMMM d, yyyy")));
    if (date == today.addDays(-1))
        return i18n::t("Yesterday, %1").arg(english.toString(date, QStringLiteral("MMMM d, yyyy")));
    return english.toString(date, QStringLiteral("dddd, MMMM d, yyyy"));
}

QString historyLabel(const QDate& date)
{
    if (!date.isValid())
        return i18n::t("History");
    return i18n::t("History \xC2\xB7 %1").arg(QLocale(QLocale::English).toString(date, QStringLiteral("MMMM d, yyyy")));
}

QDate parseDayText(const QString& input)
{
    const QString text = input.trimmed();
    if (text.isEmpty() || text.size() > 40)
        return {};
    QDate d = QLocale::system().toDate(text, QLocale::ShortFormat);
    if (d.isValid() && d.year() < 100)
        d = d.addYears(2000);
    if (d.isValid())
        return d;
    for (const char* format : {"M/d/yyyy", "d.M.yyyy", "yyyy-MM-dd", "d/M/yyyy"}) {
        d = QDate::fromString(text, QString::fromLatin1(format));
        if (d.isValid())
            return d;
    }
    return {};
}

// ============================================================================================
// Avatars
// ============================================================================================

QColor initialsColor(const QString& uid)
{
    // White initials keep at least 5:1 on each.
    static const QRgb palette[] = {0x4752c4, 0x2d7d46, 0xa8441d, 0xb83236, 0x9c3b79, 0x5d6670, 0x18788a, 0x7445ad};
    const uint        h         = qHash(uid, 0u); // the seed given: the same colour in every session
    return QColor(palette[h % 8]);
}

QImage avatarImage(const QString& nick, const QString& uid, const QImage& picture, int px, qreal dpr)
{
    QImage   img = canvas(QSize(px, px), dpr);
    QPainter p(&img);
    prepare(p);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, px, px));
    if (!picture.isNull()) {
        p.setClipPath(circle);
        const QSizeF source = picture.size() / picture.devicePixelRatio();
        const qreal  side   = qMin(source.width(), source.height());
        const QRectF crop((source.width() - side) / 2.0, (source.height() - side) / 2.0, side, side);
        const qreal  r      = picture.devicePixelRatio();
        p.drawImage(QRectF(0, 0, px, px), picture, QRectF(crop.topLeft() * r, crop.size() * r));
        return img;
    }
    p.fillPath(circle, initialsColor(uid.isEmpty() ? nick : uid));
    const QString first = firstGrapheme(nick);
    const int     id    = emoji::find(first);
    if (id >= 0 && emoji::hasColor()) {
        const int    e     = qMax(8, qRound(px * 0.62));
        const QImage image = emoji::render(id, e, dpr);
        p.drawImage(QRectF((px - e) / 2.0, (px - e) / 2.0, e, e), image);
        return img;
    }
    // fromLatin1: Qt's font cache keeps the family after we are gone (a QStringLiteral's data is in our DLL).
    QFont f = QFont(QString::fromLatin1("Segoe UI"));
    f.setPixelSize(qMax(6, qRound(0.5 * px)));
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, first.toUpper());
    return img;
}

// ============================================================================================
// Heads
// ============================================================================================

namespace {

struct ReplyFit {
    qreal   mini = 14;
    qreal   nameX = 0;
    qreal   nameW = 0;
    QString name;
    qreal   snippetX = 0;
    QString snippet;
    qreal   width = 0;
};

QFont replyNameFont(const Tokens& t, bool underline)
{
    QFont f = t.reply;
    f.setWeight(QFont::DemiBold);
    f.setUnderline(underline);
    return f;
}

// 2.3.1: an own reply row's connector on its right: the room it takes, its radius.
qreal spineRadius(const Tokens& t)
{
    return qMax(3.0, t.f / 2.0);
}

qreal ownSpineRoom(const Tokens& t)
{
    return 4 + spineRadius(t) + 6;
}

ReplyFit fitReply(const HeadSpec& spec, const Tokens& t, qreal width)
{
    ReplyFit            r;
    const QFontMetricsF nm(replyNameFont(t, false));
    const QFontMetricsF sm(t.reply);
    // 2.3.1: an own reply row starts at the picture's left edge and leaves room for its connector on the right.
    const qreal lead  = spec.ownRight ? 0 : t.G;
    const qreal tail  = spec.ownRight ? ownSpineRoom(t) : 0;
    r.mini            = qMax(10, qRound(t.f * 14 / 12.0));
    r.nameX           = lead + r.mini + 4;
    const QString at  = QLatin1Char('@') + spec.replyHeader.nick;
    const qreal   clip = spec.replyHeader.media ? 12 + 3 : 0;
    const qreal   gap  = 6;
    r.nameW           = nm.horizontalAdvance(at);
    r.width           = r.nameX + r.nameW + 2 + tail;
    if (!spec.replyHeader.snippet.isEmpty())
        r.width += gap + clip + sm.horizontalAdvance(spec.replyHeader.snippet);
    if (width <= 0)
        width = r.width;
    const qreal avail = qMax(0.0, width - r.nameX - 2 - tail);
    qreal       nameW = r.nameW;
    if (!spec.replyHeader.snippet.isEmpty() && avail - nameW - gap - clip < 48)
        nameW = qMin(nameW, qMax(avail * 0.4, avail - gap - clip - 48));
    nameW      = qMin(nameW, avail);
    r.name     = nm.elidedText(at, Qt::ElideRight, nameW);
    r.nameW    = nm.horizontalAdvance(r.name);
    r.snippetX = r.nameX + r.nameW + gap;
    const qreal room = avail - r.nameW - gap - clip;
    if (!spec.replyHeader.snippet.isEmpty() && room >= 16)
        r.snippet = sm.elidedText(spec.replyHeader.snippet, Qt::ElideRight, room);
    return r;
}

// 2.3.1: an own reply head: the reply row right-aligned before its connector, which comes down from the row's
// middle at the right into the bubble's top below the picture.
void renderOwnReply(QPainter& p, const HeadSpec& spec, const Tokens& t, const Colors& c, const QSize& size, qreal dpr)
{
    const ReplyFit      fit      = fitReply(spec, t, size.width());
    const bool          live     = spec.replyHeader.found && (spec.replyHover || spec.replyPressed);
    const QFont         nameFont = replyNameFont(t, live);
    const QFontMetricsF nm(nameFont);
    const QFontMetricsF sm(t.reply);
    const qreal         clip  = spec.replyHeader.media && !fit.snippet.isEmpty() ? 12 + 3 : 0;
    const qreal         end   = fit.snippet.isEmpty() ? fit.nameX + fit.nameW : fit.snippetX + clip + sm.horizontalAdvance(fit.snippet);
    const qreal         right = size.width() - ownSpineRoom(t);
    const qreal         shift = qMax(0.0, right - end); // flush against the connector
    const qreal         r     = spineRadius(t);
    const qreal         mid   = std::floor(t.replyRow / 2.0) + 0.5;
    const qreal         sx    = size.width() - 6.5;
    QPainterPath        spine;
    spine.moveTo(right + 4, mid);
    spine.lineTo(sx - r, mid);
    spine.quadTo(sx, mid, sx, mid + r);
    spine.lineTo(sx, size.height() + 1); // into the bubble's top (under the picture)
    p.setPen(QPen(live ? c.muted : c.spine, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(spine);
    const QRectF miniRect(shift, (t.replyRow - fit.mini) / 2.0, fit.mini, fit.mini);
    p.drawImage(miniRect, avatarImage(spec.replyHeader.nick, spec.replyHeader.uid, spec.replyAvatar, qRound(fit.mini), dpr));
    const qreal base = std::round(t.replyRow / 2.0 + (nm.ascent() - nm.descent()) / 2.0);
    p.setFont(nameFont);
    p.setPen(spec.replyName.isValid() ? spec.replyName : c.muted);
    replyart::drawRun(p, shift + fit.nameX, base, nm, fit.name);
    if (!fit.snippet.isEmpty()) {
        const QColor color = live ? c.text : c.muted;
        qreal        x     = shift + fit.snippetX;
        if (spec.replyHeader.media) {
            replyart::drawGlyph(p, replyart::Glyph::Clip, QRectF(x, base - sm.xHeight() / 2.0 - 6, 12, 12), color);
            x += 12 + 3;
        }
        p.setFont(t.reply);
        p.setPen(color);
        replyart::drawRun(p, x, base, sm, fit.snippet);
    }
}

} // namespace

qreal headNameRowWidth(const HeadSpec& spec, const Tokens& t)
{
    if (spec.ownRight)
        return 0; // 2.3.1: no name row on the right
    const QFontMetricsF nm(t.name);
    const QFontMetricsF tm(t.time);
    qreal               w = t.G + nm.horizontalAdvance(spec.nick) + 2;
    if (!spec.timeLabel.isEmpty())
        w += t.nameGap + tm.horizontalAdvance(spec.timeLabel);
    return w;
}

QSize headSize(const HeadSpec& spec, const Tokens& t, int maxWidth)
{
    if (spec.ownRight) {
        // 2.3.1: the reply row alone, its connector reaching the bubble's top padding below it (nothing without one).
        if (!spec.reply)
            return QSize(0, 0);
        const int width = qMax(t.G, qMin(maxWidth > 0 ? maxWidth : 100000, static_cast<int>(std::ceil(fitReply(spec, t, 0).width))));
        return QSize(width, t.replyRow + t.bubblePadTop + 2);
    }
    qreal w = headNameRowWidth(spec, t);
    if (spec.reply)
        w = qMax(w, fitReply(spec, t, 0).width);
    const int width  = qMax(t.G + 8, qMin(maxWidth > 0 ? maxWidth : 100000, static_cast<int>(std::ceil(w))));
    const int height = t.headRow + (spec.reply ? t.replyRow : 0);
    return QSize(width, height);
}

HeadGeometry headGeometry(const HeadSpec& spec, const Tokens& t, const QSize& size)
{
    HeadGeometry g;
    if (spec.ownRight) {
        // 2.3.1: no avatar, name or time (the time is on the bubble); the reply row is the click target.
        if (spec.reply)
            g.replyRow = QRectF(0, 0, size.width(), t.replyRow);
        return g;
    }
    const QFontMetricsF nm(t.name);
    const QFontMetricsF tm(t.time);
    const qreal         top = spec.reply ? t.replyRow : 0;
    g.avatar                = QRectF(t.avatarX, top + (t.headRow - t.avatar) / 2.0, t.avatar, t.avatar);
    g.baseline              = std::round(top + t.headRow / 2.0 + (nm.ascent() - nm.descent()) / 2.0);
    const qreal avail       = qMax(0.0, size.width() - t.G - 2.0);
    const qreal nameW       = nm.horizontalAdvance(spec.nick);
    const qreal timeW       = spec.timeLabel.isEmpty() ? 0 : tm.horizontalAdvance(spec.timeLabel);
    qreal       room        = avail;
    bool        time        = timeW > 0;
    if (time && avail - t.nameGap - timeW < qMin(nameW, 48.0))
        time = false; // too narrow: the time goes, the name stays
    if (time)
        room = avail - t.nameGap - timeW;
    g.shownName = nameW <= room ? spec.nick : nm.elidedText(spec.nick, Qt::ElideRight, room);
    g.elided    = g.shownName != spec.nick;
    const qreal shownW = nm.horizontalAdvance(g.shownName);
    g.name             = QRectF(t.G, top, shownW, t.headRow);
    if (time)
        g.time = QRectF(t.G + shownW + t.nameGap, top, timeW, t.headRow);
    if (spec.reply)
        g.replyRow = QRectF(t.avatarX + t.avatar / 2.0, 0, size.width() - t.avatarX - t.avatar / 2.0, t.replyRow);
    return g;
}

QImage renderHead(const HeadSpec& spec, const Tokens& t, const Colors& c, const QSize& size, qreal dpr)
{
    QImage   img = canvas(size, dpr);
    QPainter p(&img);
    prepare(p);
    if (spec.ownRight) {
        if (spec.reply)
            renderOwnReply(p, spec, t, c, size, dpr); // 2.3.1
        p.end();
        return img;
    }
    const HeadGeometry g = headGeometry(spec, t, size);

    if (spec.reply) {
        // Discord's reply row: the connector from the avatar's top up and to the right, the original's
        // small avatar, "@Name" and the snippet.
        const qreal  cx  = g.avatar.center().x();
        const qreal  mid = std::floor(t.replyRow / 2.0) + 0.5;
        const qreal  r   = qMax(3.0, t.f / 2.0);
        const bool   live = spec.replyHeader.found && (spec.replyHover || spec.replyPressed);
        QPainterPath spine;
        spine.moveTo(cx, g.avatar.top() - 1);
        spine.lineTo(cx, mid + r);
        spine.quadTo(cx, mid, cx + r, mid);
        spine.lineTo(t.G - 4, mid);
        p.setPen(QPen(live ? c.muted : c.spine, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(spine);
        const ReplyFit fit = fitReply(spec, t, size.width());
        const QRectF   miniRect(t.G, (t.replyRow - fit.mini) / 2.0, fit.mini, fit.mini);
        p.drawImage(miniRect, avatarImage(spec.replyHeader.nick, spec.replyHeader.uid, spec.replyAvatar, qRound(fit.mini), dpr));
        const QFont         nameFont = replyNameFont(t, live);
        const QFontMetricsF nm(nameFont);
        const QFontMetricsF sm(t.reply);
        const qreal         base = std::round(t.replyRow / 2.0 + (nm.ascent() - nm.descent()) / 2.0);
        p.setFont(nameFont);
        p.setPen(spec.replyName.isValid() ? spec.replyName : c.muted);
        replyart::drawRun(p, fit.nameX, base, nm, fit.name);
        if (!fit.snippet.isEmpty()) {
            const QColor color = live ? c.text : c.muted;
            qreal        x     = fit.snippetX;
            if (spec.replyHeader.media) {
                replyart::drawGlyph(p, replyart::Glyph::Clip, QRectF(x, base - sm.xHeight() / 2.0 - 6, 12, 12), color);
                x += 12 + 3;
            }
            p.setFont(t.reply);
            p.setPen(color);
            replyart::drawRun(p, x, base, sm, fit.snippet);
        }
    }
    if (spec.showAvatar)
        p.drawImage(g.avatar, avatarImage(spec.nick, spec.uid, spec.avatar, t.avatar, dpr));
    const QFontMetricsF nm(t.name);
    p.setFont(t.name);
    p.setPen(spec.nameColor.isValid() ? spec.nameColor : c.name);
    replyart::drawRun(p, g.name.left(), g.baseline, nm, g.shownName);
    if (!g.time.isEmpty()) {
        const QFontMetricsF tm(t.time);
        p.setFont(t.time);
        p.setPen(c.muted);
        p.drawText(QPointF(g.time.left(), g.baseline), spec.timeLabel);
    }
    p.end();
    return img;
}

// ---- compact -------------------------------------------------------------------------------------------

QSize compactHeadSize(const QString& nick, const Tokens& t, bool nameOnly, int maxWidth)
{
    const QFontMetricsF nm(t.name);
    const QFontMetricsF bm(t.body);
    const qreal         w = (nameOnly ? 0 : t.L) + nm.horizontalAdvance(nick) + t.compactNameGap;
    const int           h = static_cast<int>(std::ceil(bm.ascent() + bm.descent()));
    const int           cap = maxWidth > 0 ? maxWidth : 100000;
    return QSize(qMin(cap, static_cast<int>(std::ceil(w))), qMax(1, h));
}

QImage renderCompactHead(const QString& nick, const QColor& nameColor, const QString& time, const Tokens& t, const Colors& c, const QSize& size, bool nameOnly,
                         qreal dpr)
{
    QImage   img = canvas(size, dpr);
    QPainter p(&img);
    prepare(p);
    const qreal         base = bodyAscent(t);
    const QFontMetricsF nm(t.name);
    if (!nameOnly && !time.isEmpty()) {
        p.setFont(t.compactTime);
        p.setPen(c.muted);
        p.drawText(QPointF(0, base), time);
    }
    const qreal x    = nameOnly ? 0 : t.L;
    const qreal room = qMax(0.0, size.width() - x - t.compactNameGap);
    p.setFont(t.name);
    p.setPen(nameColor.isValid() ? nameColor : c.name);
    replyart::drawRun(p, x, base, nm, nm.elidedText(nick, Qt::ElideRight, room));
    p.end();
    return img;
}

// ============================================================================================
// System rows
// ============================================================================================

QColor systemIconColor(SystemKind kind, const Colors& c)
{
    switch (kind) {
    case SystemKind::Join:
        return c.join;
    case SystemKind::Leave:
    case SystemKind::Danger:
    case SystemKind::Warning:
        return c.leave;
    case SystemKind::Announce:
    case SystemKind::Poke:
    case SystemKind::TsMedia:
        return c.announce;
    case SystemKind::Group:
    case SystemKind::Edit:
    case SystemKind::Info:
        break;
    }
    return c.muted;
}

void drawSystemIcon(QPainter& p, SystemKind kind, const QRectF& box, const QColor& color)
{
    const qreal  s = qMin(box.width(), box.height());
    const QRectF b(box.center().x() - s / 2.0, box.center().y() - s / 2.0, s, s);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (kind) {
    case SystemKind::Join:
    case SystemKind::Leave: {
        // A door frame on the right, an arrow going in (join) or out (leave).
        QPainterPath door;
        door.moveTo(at(b, 0.56, 0.16));
        door.lineTo(at(b, 0.84, 0.16));
        door.lineTo(at(b, 0.84, 0.84));
        door.lineTo(at(b, 0.56, 0.84));
        p.drawPath(door);
        const bool  in   = kind == SystemKind::Join;
        const qreal from = in ? 0.12 : 0.66;
        const qreal to   = in ? 0.62 : 0.12;
        p.drawLine(at(b, from, 0.5), at(b, to, 0.5));
        QPainterPath head;
        const qreal dir = in ? -1 : 1;
        head.moveTo(at(b, to + dir * 0.18, 0.32));
        head.lineTo(at(b, to, 0.5));
        head.lineTo(at(b, to + dir * 0.18, 0.68));
        p.drawPath(head);
        break;
    }
    case SystemKind::Danger:
    case SystemKind::Warning: {
        QPainterPath tri;
        tri.moveTo(at(b, 0.5, 0.12));
        tri.lineTo(at(b, 0.92, 0.86));
        tri.lineTo(at(b, 0.08, 0.86));
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawLine(at(b, 0.5, 0.40), at(b, 0.5, 0.60));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(at(b, 0.5, 0.73), s * 0.055, s * 0.055);
        break;
    }
    case SystemKind::Group: {
        // A badge: a shield.
        QPainterPath shield;
        shield.moveTo(at(b, 0.5, 0.10));
        shield.lineTo(at(b, 0.84, 0.22));
        shield.cubicTo(at(b, 0.84, 0.56), at(b, 0.70, 0.78), at(b, 0.5, 0.90));
        shield.cubicTo(at(b, 0.30, 0.78), at(b, 0.16, 0.56), at(b, 0.16, 0.22));
        shield.closeSubpath();
        p.drawPath(shield);
        p.drawLine(at(b, 0.36, 0.48), at(b, 0.47, 0.59));
        p.drawLine(at(b, 0.47, 0.59), at(b, 0.66, 0.38));
        break;
    }
    case SystemKind::Edit: {
        QPainterPath pencil;
        pencil.moveTo(at(b, 0.18, 0.82));
        pencil.lineTo(at(b, 0.22, 0.64));
        pencil.lineTo(at(b, 0.66, 0.20));
        pencil.lineTo(at(b, 0.80, 0.34));
        pencil.lineTo(at(b, 0.36, 0.78));
        pencil.closeSubpath();
        p.drawPath(pencil);
        p.drawLine(at(b, 0.58, 0.28), at(b, 0.72, 0.42));
        break;
    }
    case SystemKind::Announce: {
        // A megaphone.
        QPainterPath horn;
        horn.moveTo(at(b, 0.16, 0.40));
        horn.lineTo(at(b, 0.36, 0.40));
        horn.lineTo(at(b, 0.78, 0.18));
        horn.lineTo(at(b, 0.78, 0.82));
        horn.lineTo(at(b, 0.36, 0.60));
        horn.lineTo(at(b, 0.16, 0.60));
        horn.closeSubpath();
        p.drawPath(horn);
        p.drawLine(at(b, 0.30, 0.62), at(b, 0.36, 0.84));
        break;
    }
    case SystemKind::Poke: {
        // A hand (a pointing finger).
        QPainterPath hand;
        hand.moveTo(at(b, 0.40, 0.56));
        hand.lineTo(at(b, 0.40, 0.18));
        hand.cubicTo(at(b, 0.40, 0.10), at(b, 0.52, 0.10), at(b, 0.52, 0.18));
        hand.lineTo(at(b, 0.52, 0.46));
        hand.lineTo(at(b, 0.74, 0.50));
        hand.cubicTo(at(b, 0.84, 0.52), at(b, 0.86, 0.60), at(b, 0.84, 0.68));
        hand.lineTo(at(b, 0.78, 0.88));
        hand.lineTo(at(b, 0.42, 0.88));
        hand.lineTo(at(b, 0.24, 0.66));
        hand.cubicTo(at(b, 0.20, 0.60), at(b, 0.30, 0.52), at(b, 0.40, 0.60));
        p.drawPath(hand);
        break;
    }
    case SystemKind::TsMedia: {
        // TS Media's mark: a picture frame with a hill and a sun.
        p.drawRoundedRect(QRectF(at(b, 0.12, 0.20), at(b, 0.88, 0.80)), s * 0.12, s * 0.12);
        QPainterPath hill;
        hill.moveTo(at(b, 0.20, 0.72));
        hill.lineTo(at(b, 0.42, 0.48));
        hill.lineTo(at(b, 0.58, 0.62));
        hill.lineTo(at(b, 0.66, 0.54));
        hill.lineTo(at(b, 0.80, 0.70));
        p.drawPath(hill);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(at(b, 0.66, 0.36), s * 0.07, s * 0.07);
        break;
    }
    case SystemKind::Info:
        p.drawEllipse(QRectF(at(b, 0.12, 0.12), at(b, 0.88, 0.88)));
        p.drawLine(at(b, 0.5, 0.46), at(b, 0.5, 0.70));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(at(b, 0.5, 0.32), s * 0.06, s * 0.06);
        break;
    }
    p.restore();
}

QSize systemPrefixSize(const Tokens& t)
{
    // 2.3 chat-visuals: the icon in front of a centred row's text.
    if (t.mode == Mode::Compact) {
        const QFontMetricsF bm(t.body);
        return QSize(t.systemPrefixWidth, qMax(1, static_cast<int>(std::ceil(bm.ascent() + bm.descent()))));
    }
    return QSize(t.systemPrefixWidth, t.lineMin);
}

QImage renderSystemPrefix(SystemKind kind, const Tokens& t, const Colors& c, qreal dpr)
{
    const QSize size = systemPrefixSize(t);
    QImage      img  = canvas(size, dpr);
    QPainter    p(&img);
    prepare(p);
    const QColor color = systemIconColor(kind, c);
    const qreal  side  = qMin<qreal>(qMin(16, size.height()), size.width() - 4);
    if (t.mode == Mode::Compact) {
        // Bottom-aligned on the line: the icon around the middle of the text's x-height.
        const QFontMetricsF bm(t.body);
        const qreal         mid = bodyAscent(t) - bm.xHeight() / 2.0;
        drawSystemIcon(p, kind, QRectF(0, mid - side / 2.0, side, side), color);
    } else {
        drawSystemIcon(p, kind, QRectF(0, (size.height() - side) / 2.0, side, side), color);
    }
    p.end();
    return img;
}

// ============================================================================================
// Dividers and chips
// ============================================================================================

int dividerHeight(const Tokens& t)
{
    return t.dividerHeight;
}

QImage renderDivider(const QString& label, int width, const Tokens& t, const Colors& c, qreal dpr)
{
    const int h   = t.dividerHeight;
    QImage    img = canvas(QSize(qMax(1, width), h), dpr);
    QPainter  p(&img);
    prepare(p);
    const QFontMetricsF fm(t.divider);
    const QString       text = fm.elidedText(label, Qt::ElideRight, qMax(0, width - 48));
    const qreal         tw   = fm.horizontalAdvance(text);
    const qreal         cx   = width / 2.0;
    const qreal         y    = std::floor(h / 2.0) + 0.5;
    p.setPen(QPen(c.divider, 1.0));
    p.setRenderHint(QPainter::Antialiasing, false);
    if (cx - tw / 2.0 - 8 > 0)
        p.drawLine(QPointF(0, y), QPointF(cx - tw / 2.0 - 8, y));
    if (cx + tw / 2.0 + 8 < width)
        p.drawLine(QPointF(cx + tw / 2.0 + 8, y), QPointF(width, y));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(t.divider);
    p.setPen(c.muted);
    p.drawText(QPointF(cx - tw / 2.0, std::round(h / 2.0 + (fm.ascent() - fm.descent()) / 2.0)), text);
    p.end();
    return img;
}

QSize chipSize(const QString& text, const Tokens& t)
{
    const QFontMetricsF fm(t.chip);
    return QSize(static_cast<int>(std::ceil(fm.horizontalAdvance(text) + 12)) + 2, 16);
}

QImage renderChip(const QString& text, bool hovered, const Tokens& t, const Colors& c, qreal dpr)
{
    const QSize size = chipSize(text, t);
    QImage      img  = canvas(size, dpr);
    QPainter    p(&img);
    prepare(p);
    const QRectF box = QRectF(1, 0, size.width() - 2, size.height()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(c.chipBorder, 1.0));
    p.setBrush(hovered ? QBrush(c.chipHover) : QBrush(Qt::NoBrush));
    p.drawRoundedRect(box, 8, 8);
    const QFontMetricsF fm(t.chip);
    p.setFont(t.chip);
    p.setPen(hovered ? c.text : c.muted);
    p.drawText(QPointF(1 + 6, std::round(size.height() / 2.0 + (fm.ascent() - fm.descent()) / 2.0)), text);
    p.end();
    return img;
}

QString ownTimeText(const QString& headerTime, bool ampm)
{
    return shortTime(headerTime, ampm || hasAmPm(headerTime));
}

qreal ownTimeWidth(const QString& text, const Tokens& t)
{
    return text.isEmpty() ? 0.0 : std::ceil(QFontMetricsF(t.time).horizontalAdvance(text));
}

void paintOwnTime(QPainter& p, const QString& text, const QPointF& baseline, const Tokens& t, const QColor& color)
{
    if (text.isEmpty())
        return;
    p.save();
    p.setFont(t.time);
    p.setPen(color);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.drawText(baseline, text);
    p.restore();
}

void paintGutterTime(QPainter& p, const QString& text, qreal right, qreal baseline, const Tokens& t, const Colors& c, bool compact)
{
    if (text.isEmpty())
        return;
    const QFont         font = compact ? t.compactTime : t.time;
    const QFontMetricsF fm(font);
    p.save();
    p.setFont(font);
    p.setPen(c.muted);
    p.setRenderHint(QPainter::TextAntialiasing);
    const qreal x = compact ? right : right - fm.horizontalAdvance(text);
    p.drawText(QPointF(x, baseline), text);
    p.restore();
}

qreal gutterTimeRight(const Tokens& t)
{
    // 8 px left of the text; with bubbles left of the bubble (its padding is in front of the text), with air.
    return t.bubbles ? t.G - t.bubblePadX - qMax(3, scaled(4, t.f)) : t.G - 8;
}

QString gutterTimeText(const QString& time, const Tokens& t, qreal room)
{
    const QFontMetricsF fm(t.time);
    if (fm.horizontalAdvance(time) <= room)
        return time;
    // "12:59 PM" wider than the gutter: the clock alone (the group's head shows AM or PM).
    const int space = time.lastIndexOf(QLatin1Char(' '));
    return space > 0 ? time.left(space) : time;
}

// ============================================================================================
// The action bar's glyphs
// ============================================================================================

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    const qreal  s = qMin(box.width(), box.height());
    const QRectF b(box.center().x() - s / 2.0, box.center().y() - s / 2.0, s, s);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::React: {
        p.drawEllipse(QRectF(at(b, 0.08, 0.16), at(b, 0.76, 0.84)));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(at(b, 0.31, 0.41), s * 0.05, s * 0.05);
        p.drawEllipse(at(b, 0.53, 0.41), s * 0.05, s * 0.05);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath smile;
        smile.moveTo(at(b, 0.27, 0.57));
        smile.quadTo(at(b, 0.42, 0.72), at(b, 0.57, 0.57));
        p.drawPath(smile);
        p.drawLine(at(b, 0.86, 0.04), at(b, 0.86, 0.30));
        p.drawLine(at(b, 0.73, 0.17), at(b, 0.99, 0.17));
        break;
    }
    case Glyph::Reply: {
        QPainterPath head;
        head.moveTo(at(b, 0.40, 0.20));
        head.lineTo(at(b, 0.14, 0.45));
        head.lineTo(at(b, 0.40, 0.70));
        p.drawPath(head);
        QPainterPath shaft;
        shaft.moveTo(at(b, 0.16, 0.45));
        shaft.lineTo(at(b, 0.58, 0.45));
        shaft.cubicTo(at(b, 0.80, 0.45), at(b, 0.88, 0.58), at(b, 0.88, 0.82));
        p.drawPath(shaft);
        break;
    }
    case Glyph::Copy:
        p.drawRoundedRect(QRectF(at(b, 0.32, 0.30), at(b, 0.86, 0.88)), s * 0.08, s * 0.08);
        p.drawPolyline(QPolygonF() << at(b, 0.20, 0.70) << at(b, 0.14, 0.70) << at(b, 0.14, 0.12) << at(b, 0.66, 0.12) << at(b, 0.66, 0.18));
        break;
    case Glyph::More:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (const qreal x : {0.2, 0.5, 0.8})
            p.drawEllipse(at(b, x, 0.5), s * 0.075, s * 0.075);
        break;
    case Glyph::Check: {
        QPainterPath check;
        check.moveTo(at(b, 0.18, 0.52));
        check.lineTo(at(b, 0.40, 0.74));
        check.lineTo(at(b, 0.84, 0.28));
        pen.setWidthF(1.8);
        p.setPen(pen);
        p.drawPath(check);
        break;
    }
    }
    p.restore();
}

} // namespace layoutart
