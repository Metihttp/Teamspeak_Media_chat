#include "pollart.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QLocale>
#include <QTextLayout>
#include <QtMath>

#include <cmath>

#include "emojitext.h"
#include "i18n.h"
#include "uiutil.h"

namespace pollart {

namespace {

constexpr int   kMaxCard     = 400;
constexpr int   kMinCard     = 240;
constexpr qreal kPadX        = 14.0;
constexpr qreal kPadTop      = 12.0;
constexpr qreal kPadBottom   = 6.0;
constexpr qreal kRadius      = 12.0;
constexpr qreal kMark        = 18.0; // the circle / box before an option
constexpr qreal kMarkGap     = 10.0;
constexpr qreal kRowInset    = 6.0;  // a row's hover reaches this far past the text
constexpr qreal kBar         = 5.0;
constexpr qreal kFooter      = 34.0;
constexpr int   kQuestionLines = 8;
constexpr int   kOptionLines   = 5;

struct Colors {
    QColor card, border, text, muted, accent, fill, track, hover, pressed, error, onFill;
};

// 2.3: on a message bubble (Style::bubbles) the bubble is the card's panel: no panel of its own, and the
// content starts at the bubble's text column (a row's hover reaches a little past it).
struct Pad {
    qreal x, top, bottom, rowInset;
};

Pad padOf(const Style& s)
{
    if (!s.bubbles.isEmpty())
        return {2.0, 2.0, 0.0, 2.0};
    return {kPadX, kPadTop, kPadBottom, kRowInset};
}

// "8,120": the same grouping as "12,345 votes" (English-only UI).
QString countText(int n)
{
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(n);
}

QColor withAlpha(QColor c, qreal a)
{
    c.setAlphaF(a);
    return c;
}

Colors colorsFor(const Style& s)
{
    Colors c;
    const QColor base = s.base.isValid() ? s.base : (s.dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff));
    c.text            = s.text.isValid() ? s.text : (s.dark ? QColor(0xdc, 0xdd, 0xde) : QColor(0x06, 0x06, 0x07));
    if (s.dark) {
        c.card    = ui::flatten(QColor(255, 255, 255, 15), base);
        c.border  = ui::flatten(QColor(255, 255, 255, 26), base);
        c.accent  = QColor(0xa5, 0xab, 0xf8);
        c.fill    = QColor(0x7c, 0x85, 0xf6);
        c.track   = ui::flatten(QColor(255, 255, 255, 26), c.card);
        c.hover   = QColor(255, 255, 255, 16);
        c.pressed = QColor(255, 255, 255, 28);
        c.error   = QColor(0xfa, 0x77, 0x7c);
    } else {
        c.card    = ui::flatten(QColor(0, 0, 0, 8), base);
        c.border  = ui::flatten(QColor(0, 0, 0, 30), base);
        c.accent  = QColor(0x47, 0x52, 0xc4);
        c.fill    = QColor(0x58, 0x65, 0xf2);
        c.track   = ui::flatten(QColor(0, 0, 0, 22), c.card);
        c.hover   = QColor(0, 0, 0, 12);
        c.pressed = QColor(0, 0, 0, 22);
        c.error   = QColor(0xc4, 0x28, 0x2d);
    }
    c.muted = ui::flatten(withAlpha(c.text, 0.64), c.card);
    if (ui::contrastRatio(c.muted, c.card) < 4.5)
        c.muted = ui::flatten(withAlpha(c.text, 0.78), c.card);
    if (ui::contrastRatio(c.accent, c.card) < 4.5)
        c.accent = c.text;
    c.onFill = QColor(0xff, 0xff, 0xff);
    if (!s.bubbles.isEmpty()) {
        // 2.3: on a bubble: no panel, no frame; translucent tracks, texts and bars at their contrast on every
        // bubble colour (yours and someone else's, hovered, mentioning you).
        const QVector<QColor>& backs = s.bubbles;
        c.card   = QColor(0, 0, 0, 0);
        c.border = QColor(0, 0, 0, 0);
        c.track  = s.dark ? QColor(255, 255, 255, 30) : QColor(6, 10, 22, 26);
        c.muted  = ui::readableOn(ui::flatten(withAlpha(c.text, 0.64), backs.first()), backs, 4.5);
        c.accent = ui::readableOn(c.accent, backs, 4.5);
        c.error  = ui::readableOn(c.error, backs, 4.5);
        QVector<QColor> tracks;
        for (const QColor& back : backs)
            tracks.append(ui::flatten(c.track, back));
        c.fill = ui::readableOn(c.fill, tracks, 3.0);
    }
    // 2.3 ux review: the Vote button's text and the checks on the fill. White where it reads (4.5:1, the light
    // skins); on the light fills of dark skins (3.2:1, 2.1:1 on a bubble) a dark ink instead, as dark themes do.
    if (ui::contrastRatio(c.onFill, c.fill) < 4.5)
        c.onFill = ui::readableOn(QColor(0x1a, 0x1b, 0x2e), {c.fill}, 4.5);
    return c;
}

// The Vote button's fill under the pointer: towards more contrast with its text, never less.
QColor buttonFill(const Colors& col, bool hovered, bool pressed)
{
    const bool lightInk = col.onFill.lightness() > 128;
    if (pressed)
        return lightInk ? col.fill.darker(118) : col.fill.lighter(112);
    if (hovered)
        return lightInk ? col.fill.darker(108) : col.fill.lighter(106);
    return col.fill;
}

QFont scaled(const QFont& base, qreal factor, bool bold)
{
    QFont f(base);
    if (base.pixelSize() > 0)
        f.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        f.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    f.setBold(bold);
    return f;
}

struct Fonts {
    QFont question, body, small, percent, button;
};

Fonts fontsFor(const Style& s)
{
    QFont base = s.font;
    if (base.pointSizeF() <= 0 && base.pixelSize() <= 0)
        base.setPointSizeF(9);
    Fonts f;
    f.question = scaled(base, 1.06, true);
    f.body     = scaled(base, 1.0, false);
    f.small    = scaled(base, 0.9, false);
    f.percent  = scaled(base, 0.95, true);
    f.button   = scaled(base, 0.95, true);
    return f;
}

// Text broken into lines that fit width (at most maxLines; the last one elided).
struct Wrapped {
    QStringList    lines;
    QVector<qreal> widths;
    qreal          lineHeight = 0;
    bool           rtl        = false;
    qreal          height() const { return lines.size() * lineHeight; }
};

Wrapped wrap(const QString& text, const QFont& font, qreal width, int maxLines)
{
    Wrapped            w;
    const QFontMetricsF fm(font);
    w.lineHeight = fm.lineSpacing();
    w.rtl        = text.isRightToLeft();
    if (text.isEmpty() || width <= 4) {
        w.lines << QString();
        w.widths << 0;
        return w;
    }
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setTextDirection(w.rtl ? Qt::RightToLeft : Qt::LeftToRight);
    layout.setTextOption(option);
    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (w.lines.size() + 1 == maxLines) {
            // The last line we show: the rest of the text, elided.
            const QString rest = text.mid(line.textStart()).trimmed();
            const QString shown = fm.elidedText(rest, Qt::ElideRight, width);
            w.lines << shown;
            w.widths << qMin(width, fm.horizontalAdvance(shown));
            break;
        }
        const QString part = text.mid(line.textStart(), line.textLength()).trimmed();
        w.lines << part;
        w.widths << qMin(width, fm.horizontalAdvance(part));
    }
    layout.endLayout();
    if (w.lines.isEmpty()) {
        w.lines << QString();
        w.widths << 0;
    }
    return w;
}

void drawWrapped(QPainter& p, const Wrapped& w, const QFont& font, const QRectF& box, const QColor& color)
{
    p.setFont(font);
    p.setPen(color);
    const QFontMetricsF fm(font);
    for (int i = 0; i < w.lines.size(); ++i) {
        const qreal baseline = box.top() + i * w.lineHeight + fm.ascent();
        const qreal x        = w.rtl ? box.right() - w.widths.at(i) : box.left();
        emoji::drawTextLine(p, x, baseline, fm, w.lines.at(i), w.rtl);
    }
}

void drawCheck(QPainter& p, const QRectF& box, const QColor& color, qreal width)
{
    QPainterPath check;
    check.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
    check.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.70);
    check.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.33);
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(check);
}

void drawInfo(QPainter& p, const QPointF& center, qreal r, const QColor& color, bool alert)
{
    p.setPen(QPen(color, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, r, r);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    if (alert) {
        p.drawRoundedRect(QRectF(center.x() - 0.75, center.y() - r * 0.55, 1.5, r * 0.65), 0.75, 0.75);
        p.drawEllipse(QPointF(center.x(), center.y() + r * 0.45), 0.9, 0.9);
    } else {
        p.drawEllipse(QPointF(center.x(), center.y() - r * 0.45), 0.9, 0.9);
        p.drawRoundedRect(QRectF(center.x() - 0.75, center.y() - r * 0.15, 1.5, r * 0.65), 0.75, 0.75);
    }
}

// What the card shows in its footer and note line.
struct Parts {
    bool    submit  = false; // the Vote button
    bool    retract = false;
    bool    retry   = false;
    QString note;
    bool    noteError = false;
};

Parts partsOf(const Card& c)
{
    Parts parts;
    if (c.failed) {
        parts.note      = i18n::t("Your vote didn't reach the others.");
        parts.noteError = true;
        parts.retry     = c.canVote;
    } else if (!c.canVote && !c.note.isEmpty()) {
        parts.note = c.note;
    }
    if (c.canVote && !c.failed) {
        if (!c.results && c.multiple)
            parts.submit = true;
        else if (c.results && !c.sending && !c.locked)
            parts.retract = true;
    }
    return parts;
}

QString actionText(Zone zone)
{
    switch (zone) {
    case Zone::Submit:
        return i18n::t("Vote");
    case Zone::Retract:
        return i18n::t("Retract vote");
    case Zone::Retry:
        return i18n::t("Retry");
    default:
        return {};
    }
}

struct Measured {
    Layout           layout;
    Wrapped          question;
    QVector<Wrapped> options;
    Wrapped          note;
    QVector<QRectF>  texts; // per option: where its text goes
    QVector<QRectF>  marks; // before voting: the circle or box; after: the percentage column
    QVector<QRectF>  bars;
    qreal            percentWidth = 0;
};

Measured measure(const Card& c, const Style& s)
{
    Measured    m;
    const Fonts f     = fontsFor(s);
    const int   width = qBound(qMin(kMinCard, qMax(160, s.maxWidth)), s.maxWidth, kMaxCard);
    const Pad   pad   = padOf(s);
    const qreal inner = width - 2 * pad.x;
    qreal       y     = pad.top;

    m.question          = wrap(c.question, f.question, inner, kQuestionLines);
    m.layout.question   = QRectF(pad.x, y, inner, m.question.height());
    y += m.question.height() + 2;
    const QFontMetricsF small(f.small);
    m.layout.badge = QRectF(pad.x, y, qMin(inner, small.horizontalAdvance(badgeText(c.multiple)) + 2), small.height());
    y += small.height() + 8;

    const QFontMetricsF percent(f.percent);
    m.percentWidth = percent.horizontalAdvance(QStringLiteral("100%")) + 2;
    for (int i = 0; i < c.options.size(); ++i) {
        if (!c.results) {
            const qreal   textLeft = pad.x + kMark + kMarkGap;
            const Wrapped w        = wrap(c.options.at(i), f.body, inner - kMark - kMarkGap, kOptionLines);
            const qreal   rowH     = qMax<qreal>(36.0, w.height() + 16.0);
            const QRectF  row(pad.x - pad.rowInset, y, inner + 2 * pad.rowInset, rowH);
            const qreal   textTop = y + (rowH - w.height()) / 2.0;
            m.options.append(w);
            m.texts.append(QRectF(textLeft, textTop, inner - kMark - kMarkGap, w.height()));
            m.marks.append(QRectF(pad.x, textTop + w.lineHeight / 2.0 - kMark / 2.0, kMark, kMark));
            m.bars.append(QRectF());
            m.layout.rows.append(row);
            y += rowH + 2;
        } else {
            const QString count     = countText(c.counts.value(i));
            const qreal   countW    = small.horizontalAdvance(count) + 2;
            const qreal   textLeft  = pad.x + m.percentWidth + 10;
            const qreal   textWidth = inner - m.percentWidth - 10 - countW - 16; // room before the count (right-to-left text ends there)
            const Wrapped w         = wrap(c.options.at(i), f.body, textWidth, kOptionLines);
            const qreal   top       = y + 6;
            m.options.append(w);
            m.texts.append(QRectF(textLeft, top, textWidth, w.height()));
            m.marks.append(QRectF(pad.x, top, m.percentWidth, w.lineHeight));
            const qreal barTop = top + w.height() + 5;
            m.bars.append(QRectF(textLeft, barTop, inner - m.percentWidth - 10, kBar));
            const qreal rowH = barTop + kBar + 7 - y;
            m.layout.rows.append(QRectF(pad.x - pad.rowInset, y, inner + 2 * pad.rowInset, rowH));
            y += rowH + 2;
        }
    }

    const Parts parts = partsOf(c);
    if (!parts.note.isEmpty()) {
        y += 4;
        m.note        = wrap(parts.note, f.small, inner - 22, 3);
        m.layout.note = QRectF(pad.x, y, inner, qMax<qreal>(18.0, m.note.height()));
        y += m.layout.note.height() + 2;
    }

    m.layout.footer = QRectF(pad.x, y, inner, kFooter);
    const QFontMetricsF button(f.button);
    QString             voters = votesText(c.voters);
    if (c.sending)
        voters += QString::fromUtf8(" \xC2\xB7 ") + i18n::t("Sending\xE2\x80\xA6");
    const qreal votersW = small.horizontalAdvance(voters) + 2;
    Zone        action  = parts.submit ? Zone::Submit : parts.retract ? Zone::Retract : parts.retry ? Zone::Retry : Zone::None;
    if (action != Zone::None) {
        const qreal w = button.horizontalAdvance(actionText(action)) + 28;
        const qreal h = 26;
        if (action == Zone::Submit) // like Telegram: the Vote button in the middle, nothing else
            m.layout.action = QRectF(pad.x + (inner - w) / 2.0, y + (kFooter - h) / 2.0, w, h);
        else
            m.layout.action = QRectF(pad.x + inner - w + qMin<qreal>(8.0, pad.x), y + (kFooter - h) / 2.0, w, h);
        m.layout.actionZone = action;
    }
    if (action == Zone::Submit)
        m.layout.votes = QRectF();
    else if (action == Zone::None)
        m.layout.votes = QRectF(pad.x + (inner - votersW) / 2.0, y, votersW, kFooter);
    else
        m.layout.votes = QRectF(pad.x, y, qMin(votersW, m.layout.action.left() - pad.x - 6), kFooter);
    y += kFooter + pad.bottom;

    m.layout.size = QSize(width, qCeil(y));
    m.layout.card = QRectF(0, 0, width, m.layout.size.height());
    return m;
}

} // namespace

ButtonColors voteButtonColors(const Style& style, bool hovered, bool pressed)
{
    const Colors col = colorsFor(style);
    return {buttonFill(col, hovered, pressed), col.onFill};
}

QString votesText(int voters)
{
    if (voters <= 0)
        return i18n::t("No votes yet");
    if (voters == 1)
        return i18n::t("1 vote");
    return i18n::t("%1 votes").arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(voters)); // "1,250 votes" (English-only UI)
}

QString badgeText(bool multiple)
{
    return multiple ? i18n::t("Anonymous poll \xC2\xB7 Multiple answers") : i18n::t("Anonymous poll");
}

QString badgeToolTip()
{
    return i18n::t("Nobody can see who voted for what, not even the person who made the poll.");
}

QString describe(const Card& c)
{
    QStringList parts;
    parts << i18n::t("Poll: %1").arg(c.question) << badgeText(c.multiple);
    for (int i = 0; i < c.options.size(); ++i) {
        QString line = QString::number(i + 1) + QLatin1String(". ") + c.options.at(i);
        if (c.results)
            line += QString::fromLatin1(" (%1%, %2)").arg(c.percents.value(i)).arg(c.counts.value(i));
        if (c.results && (c.own & (1u << i)))
            line += QLatin1Char(' ') + i18n::t("(your vote)");
        parts << line;
    }
    parts << votesText(c.voters);
    return parts.join(QLatin1Char('\n'));
}

Layout layout(const Card& card, const Style& style)
{
    return measure(card, style).layout;
}

void paint(QPainter& p, const Card& c, const Style& s, const Layout&, const Pointer& pointer)
{
    const Measured m     = measure(c, s);
    const Layout&  l     = m.layout;
    const Colors   col   = colorsFor(s);
    const Fonts    fonts = fontsFor(s);
    const Parts    parts = partsOf(c);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // The panel (2.3: on a bubble, the bubble is the panel).
    if (col.card.alpha() > 0) {
        p.setPen(QPen(col.border, 1.0));
        p.setBrush(col.card);
        p.drawRoundedRect(l.card.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    }

    drawWrapped(p, m.question, fonts.question, l.question, col.text);
    p.setFont(fonts.small);
    p.setPen(col.muted);
    p.drawText(l.badge, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
               QFontMetricsF(fonts.small).elidedText(badgeText(c.multiple), Qt::ElideRight, l.badge.width()));

    const QFontMetricsF small(fonts.small);
    for (int i = 0; i < c.options.size(); ++i) {
        const QRectF row      = l.rows.at(i);
        const bool   hovered  = pointer.hover == Zone::Option && pointer.hoverIndex == i;
        const bool   pressed  = pointer.pressed == Zone::Option && pointer.pressedIndex == i;
        const bool   mine     = c.results && (c.own & (1u << i));
        if (!c.results) {
            const bool active   = c.canVote && !c.sending;
            const bool selected = c.multiple && (c.selected & (1u << i));
            if (active && (hovered || pressed)) {
                p.setPen(Qt::NoPen);
                p.setBrush(pressed ? col.pressed : col.hover);
                p.drawRoundedRect(row, 8, 8);
            }
            const QRectF mark = m.marks.at(i).adjusted(1, 1, -1, -1);
            const QColor ring = !active ? withAlpha(col.muted, 0.55) : hovered ? col.accent : col.muted;
            if (c.multiple) {
                if (selected) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(col.fill);
                    p.drawRoundedRect(mark, 4.5, 4.5);
                    drawCheck(p, mark, col.onFill, 1.9);
                } else {
                    p.setPen(QPen(ring, 1.6));
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(mark.adjusted(0.8, 0.8, -0.8, -0.8), 4.0, 4.0);
                }
            } else {
                p.setPen(QPen(ring, 1.6));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(mark.adjusted(0.8, 0.8, -0.8, -0.8));
                if (pressed && active) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(col.fill);
                    p.drawEllipse(mark.center(), 4.0, 4.0);
                }
            }
            drawWrapped(p, m.options.at(i), fonts.body, m.texts.at(i), active ? col.text : col.muted);
            continue;
        }
        // Results: the percentage, the text, the count, the bar; a check on your choice.
        const QRectF pcol = m.marks.at(i);
        p.setFont(fonts.percent);
        p.setPen(col.text);
        p.drawText(QRectF(pcol.left(), pcol.top(), pcol.width(), pcol.height()), Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine,
                   QString::number(c.percents.value(i)) + QLatin1Char('%'));
        drawWrapped(p, m.options.at(i), fonts.body, m.texts.at(i), col.text);
        p.setFont(fonts.small);
        p.setPen(col.muted);
        const QRectF countBox(m.texts.at(i).right() + 4, m.texts.at(i).top(), l.card.width() - padOf(s).x - m.texts.at(i).right() - 4, m.options.at(i).lineHeight);
        p.drawText(countBox, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, countText(c.counts.value(i)));
        const QRectF bar = m.bars.at(i);
        p.setPen(Qt::NoPen);
        p.setBrush(col.track);
        p.drawRoundedRect(bar, kBar / 2.0, kBar / 2.0);
        const qreal fraction = c.voters > 0 ? qBound(0.0, static_cast<qreal>(c.counts.value(i)) / c.voters, 1.0) : 0.0;
        if (fraction > 0) {
            p.setBrush(mine ? col.fill : withAlpha(col.fill, s.dark ? 0.62 : 0.55));
            p.drawRoundedRect(QRectF(bar.left(), bar.top(), qMax(kBar, bar.width() * fraction), kBar), kBar / 2.0, kBar / 2.0);
        }
        if (mine) {
            const qreal  d = 15.0;
            const QRectF badge(pcol.right() - d, bar.center().y() - d / 2.0, d, d);
            if (col.card.alpha() > 0) {
                p.setPen(QPen(col.card, 1.5));
            } else { // on a bubble: the ring cut out, the bubble shows through
                p.save();
                p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0));
                p.drawEllipse(badge.adjusted(-1.5, -1.5, 1.5, 1.5));
                p.restore();
                p.setPen(Qt::NoPen);
            }
            p.setBrush(col.fill);
            p.drawEllipse(badge);
            drawCheck(p, badge, col.onFill, 1.6);
        }
    }

    if (!l.note.isEmpty()) {
        const QColor color = parts.noteError ? col.error : col.muted;
        drawInfo(p, QPointF(l.note.left() + 7, l.note.top() + qMax(9.0, m.note.lineHeight / 2.0)), 6.5, color, parts.noteError);
        drawWrapped(p, m.note, fonts.small, QRectF(l.note.left() + 20, l.note.top() + qMax(0.0, (18.0 - m.note.lineHeight) / 2.0), l.note.width() - 20, m.note.height()),
                    color);
    }

    // The footer.
    if (!l.votes.isEmpty()) {
        QString voters = votesText(c.voters);
        if (c.sending)
            voters += QString::fromUtf8(" \xC2\xB7 ") + i18n::t("Sending\xE2\x80\xA6");
        p.setFont(fonts.small);
        p.setPen(col.muted);
        p.drawText(l.votes, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, small.elidedText(voters, Qt::ElideRight, l.votes.width()));
    }
    if (l.actionZone != Zone::None) {
        const bool    hovered = pointer.hover == l.actionZone;
        const bool    pressed = pointer.pressed == l.actionZone;
        const QString text    = actionText(l.actionZone);
        p.setFont(fonts.button);
        if (l.actionZone == Zone::Submit) {
            const bool   enabled = c.selected != 0;
            const QColor fill    = enabled ? voteButtonColors(s, hovered, pressed).fill : withAlpha(col.muted, 0.25);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(l.action, l.action.height() / 2.0, l.action.height() / 2.0);
            p.setPen(enabled ? col.onFill : col.muted);
            p.drawText(l.action, Qt::AlignCenter | Qt::TextSingleLine, text);
        } else {
            const QColor tone = l.actionZone == Zone::Retry ? col.error : col.accent; // the hover tint matches the text
            if (hovered || pressed) {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(tone, pressed ? 0.22 : 0.13));
                p.drawRoundedRect(l.action, l.action.height() / 2.0, l.action.height() / 2.0);
            }
            p.setPen(tone);
            p.drawText(l.action, Qt::AlignCenter | Qt::TextSingleLine, text);
        }
    }
    p.restore();
}

QImage render(const Card& card, const Style& style, const Pointer& pointer, Layout* layoutOut)
{
    const Layout l   = layout(card, style);
    const qreal  dpr = qMax<qreal>(1.0, style.dpr);
    QImage       img(QSize(qCeil(l.size.width() * dpr), qCeil(l.size.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    paint(p, card, style, l, pointer);
    p.end();
    if (layoutOut)
        *layoutOut = l;
    return img;
}

Hit hitTest(const Card& c, const Layout& l, const QPointF& local)
{
    Hit hit;
    if (!l.card.contains(local))
        return hit;
    if (!l.action.isEmpty() && l.action.adjusted(-2, -2, 2, 2).contains(local)) {
        if (l.actionZone == Zone::Submit && c.selected == 0)
            return hit; // nothing ticked yet
        hit.zone = l.actionZone;
        return hit;
    }
    if (!c.results && c.canVote && !c.sending) {
        for (int i = 0; i < l.rows.size(); ++i) {
            if (l.rows.at(i).contains(local)) {
                hit.zone  = Zone::Option;
                hit.index = i;
                return hit;
            }
        }
    }
    if (l.badge.adjusted(-2, -2, 2, 2).contains(local))
        hit.zone = Zone::Badge;
    return hit;
}

} // namespace pollart
