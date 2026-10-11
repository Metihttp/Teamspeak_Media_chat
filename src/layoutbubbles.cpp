#include "layoutbubbles.h"

#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <climits>
#include <cmath>

#include "layoutformat.h"

namespace layoutbubbles {

namespace {

using namespace layoutformat;

// The next visible block (hidden ones: an album's other items, a folded run).
QTextBlock nextVisible(const QTextBlock& block)
{
    QTextBlock b = block.next();
    while (b.isValid() && !b.isVisible())
        b = b.next();
    return b;
}

QTextBlock previousVisible(const QTextBlock& block)
{
    QTextBlock b = block.previous();
    while (b.isValid() && !b.isVisible())
        b = b.previous();
    return b;
}

bool isContinuation(const QTextBlock& block)
{
    if (block.blockFormat().intProperty(kKind) != RowMessage)
        return false;
    const Lead lead = leadOf(block);
    return lead.styled && lead.kind == Continuation;
}

// Whether a line holds only pictures marked kBelowBubble (and line breaks): it stays under the bubble.
bool belowBubble(const QTextBlock& block, const QTextLine& line)
{
    const int from = block.position() + line.textStart();
    const int to   = from + line.textLength();
    bool      any  = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid() || f.position() + f.length() <= from || f.position() >= to)
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (cf.isImageFormat() && cf.boolProperty(kBelowBubble)) {
            any = true;
            continue;
        }
        const QString text = f.text().mid(qMax(0, from - f.position()), qMin(to, f.position() + f.length()) - qMax(from, f.position()));
        for (const QChar c : text) {
            if (c != QChar::LineSeparator && c != QChar::ParagraphSeparator && !c.isSpace())
                return false;
        }
    }
    return any;
}

// Whether a message block's last line stays under its bubble (its bubble's bottom corners are round).
bool endsBelowBubble(const QTextBlock& block)
{
    const QTextLayout* tl = block.isValid() ? block.layout() : nullptr;
    return tl && tl->lineCount() > 1 && belowBubble(block, tl->lineAt(tl->lineCount() - 1));
}

} // namespace

Item itemOf(const QTextBlock& block, const layoutdoc::Env& env)
{
    Item                     item;
    const layoutart::Tokens& t = env.tokens;
    if (!t.bubbles || !block.isValid() || !block.isVisible())
        return item;
    const QTextLayout* tl = block.layout();
    if (!tl || tl->lineCount() <= 0)
        return item;
    const QTextDocument* doc = block.document();
    const int            row = block.blockFormat().intProperty(kKind);
    if (row != RowMessage && row != RowSystem)
        return item;
    const Lead lead = leadOf(block);
    if (!lead.styled)
        return item;
    // Where the layout's lines are: blockBoundingRect's top left is the layout's position (its lines' origin).
    const QRectF  br     = doc->documentLayout()->blockBoundingRect(block);
    const QPointF origin = br.topLeft();
    const int     lines  = tl->lineCount();
    item.block           = block.blockNumber();

    if (row == RowSystem && lead.kind == SystemPrefix) {
        // A pill round the centred text (every line; a long row wraps).
        QRectF text;
        for (int i = 0; i < lines; ++i) {
            const QRectF r = tl->lineAt(i).naturalTextRect();
            text           = text.isNull() ? r : text.united(r);
        }
        if (text.isEmpty())
            return Item();
        item.kind = Kind::Pill;
        item.rect = text.translated(origin).adjusted(-t.pillPadX, -t.pillPadY, t.pillPadX, t.pillPadY);
        return item;
    }
    if (row != RowMessage || (lead.kind != Head && lead.kind != Continuation))
        return Item();

    if (isOwnRight(block)) {
        // 2.3.1: your message on the right: round what it shows (its lines, its time), flush right; at least
        // bubbleMinWidth wide (to the left).
        const layoutdoc::OwnPlace place = layoutdoc::ownPlace(block, env);
        if (!place.valid)
            return Item();
        const qreal right  = place.right + t.bubblePadX;
        const qreal left   = qMin(place.left - t.bubblePadX, right - t.bubbleMinWidth);
        const qreal top    = place.top - t.bubblePadTop;
        const qreal bottom = place.bottom + t.bubblePadBottom;
        if (right <= left || bottom <= top)
            return Item();
        item.kind  = Kind::Bubble;
        item.rect  = QRectF(QPointF(std::floor(left), std::floor(top)), QPointF(std::ceil(right), std::ceil(bottom)));
        item.own   = true;
        item.right = true;
        if (place.time != TimeNone) {
            item.time         = place.timeText;
            item.timeBaseline = place.timeBaseline;
            item.timeRect     = place.timeRect;
        }
        item.joinAbove = lead.kind == Continuation && !endsBelowBubble(previousVisible(block));
        item.joinBelow = isContinuation(nextVisible(block));
        return item;
    }

    // Lines at the end that hold only pictures marked kBelowBubble (a reaction row) stay outside.
    int inside = lines;
    while (inside > 1 && belowBubble(block, tl->lineAt(inside - 1)))
        --inside;
    const QTextLine first = tl->lineAt(0);
    const QTextLine last  = tl->lineAt(inside - 1);
    qreal           top   = origin.y() + first.y() - t.bubblePadTop;
    qreal           right = 0;
    int             from  = 0;
    if (lead.kind == Head) {
        // The name row: the head picture's lower part (a reply head's reply row is above it, outside).
        const QTextImageFormat image   = lead.format.toImageFormat();
        const qreal            picture = origin.y() + first.y() + first.ascent() - image.height();
        const qreal            reply   = qMax(0.0, image.height() - t.headRow);
        top                            = picture + reply - t.bubblePadHead;
        const qreal nameRow            = lead.format.hasProperty(kNameRow) ? lead.format.doubleProperty(kNameRow) : image.width();
        right                          = first.naturalTextRect().left() + qMin(nameRow, image.width());
        from                           = 1; // the head's line holds only the picture (and line breaks)
    }
    for (int i = from; i < inside; ++i)
        right = qMax(right, tl->lineAt(i).naturalTextRect().right());
    const qreal left    = origin.x() + t.G - t.bubblePadX;
    const qreal column  = origin.x() + first.x() + first.width(); // the text column's right edge
    qreal       outside = origin.x() + right + t.bubblePadX;
    outside             = qMax(outside, left + t.bubbleMinWidth);
    outside             = qMin(outside, column + t.bubblePadX);
    const qreal bottom  = origin.y() + last.y() + last.height() + t.bubblePadBottom;
    if (outside <= left || bottom <= top)
        return Item();
    item.kind    = Kind::Bubble;
    item.rect    = QRectF(QPointF(std::floor(left), std::floor(top)), QPointF(std::ceil(outside), std::ceil(bottom)));
    item.own     = block.blockFormat().boolProperty(kOwn);
    item.mention = block.blockFormat().boolProperty(kMention);
    item.joinAbove = lead.kind == Continuation && !endsBelowBubble(previousVisible(block)); // (a reaction row above: rounded)
    item.joinBelow = inside == lines && isContinuation(nextVisible(block)); // (a reaction row below: rounded)
    return item;
}

QVector<Item> itemsIn(QTextDocument* doc, int from, int to, const layoutdoc::Env& env)
{
    QVector<Item> out;
    if (!doc || !env.tokens.bubbles)
        return out;
    from = qMax(0, from - 1);
    to   = qMin(doc->blockCount() - 1, to + 1);
    for (QTextBlock b = doc->findBlockByNumber(from); b.isValid() && b.blockNumber() <= to; b = b.next()) {
        const Item item = itemOf(b, env);
        if (item.kind != Kind::None)
            out.append(item);
    }
    return out;
}

QVector<QRectF> keepOut(QTextDocument* doc, int from, int to, const layoutdoc::Env& env)
{
    QVector<QRectF> out;
    if (!doc)
        return out;
    from = qMax(0, from);
    to   = qMin(doc->blockCount() - 1, to);
    for (QTextBlock b = doc->findBlockByNumber(from); b.isValid() && b.blockNumber() <= to; b = b.next()) {
        const QTextLayout* tl  = b.isVisible() ? b.layout() : nullptr;
        const int          row = b.blockFormat().intProperty(kKind);
        if (!tl || tl->lineCount() <= 0 || (row != RowMessage && row != RowSystem))
            continue;
        const QPointF origin = doc->documentLayout()->blockBoundingRect(b).topLeft();
        const Lead    lead   = leadOf(b);
        int           first  = 0;
        if (row == RowMessage && isOwnRight(b)) {
            // 2.3.1: on the right: every line as it is (a reply row's picture too), and the time.
            const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
            if (place.valid && place.time != TimeNone && !place.timeRect.isEmpty())
                out.append(place.timeRect);
        } else if (row == RowMessage && lead.styled && lead.kind == Head) {
            // The head's line holds only its picture: [reply row] name row. The name row's text is the name and
            // the time (kNameRow wide); a reply row above it, as wide as the picture.
            const QTextLine        line    = tl->lineAt(0);
            const QTextImageFormat image   = lead.format.toImageFormat();
            const qreal            bottom  = origin.y() + line.y() + line.ascent();
            const qreal            top     = bottom - image.height();
            const qreal            left    = origin.x() + line.naturalTextRect().left();
            const qreal            nameRow = qMin(image.width(), lead.format.hasProperty(kNameRow) ? lead.format.doubleProperty(kNameRow) : image.width());
            const qreal            reply   = qMax(0.0, image.height() - env.tokens.headRow);
            if (reply > 0)
                out.append(QRectF(left, top, image.width(), reply));
            if (nameRow > 0)
                out.append(QRectF(left, top + reply, nameRow, image.height() - reply));
            first = 1;
        }
        for (int i = first; i < tl->lineCount(); ++i) {
            const QRectF r = tl->lineAt(i).naturalTextRect();
            if (r.width() > 0 && r.height() > 0)
                out.append(r.translated(origin));
        }
    }
    return out;
}

QVector<QRectF> keepOutNear(QTextDocument* doc, int block, qreal reach, const layoutdoc::Env& env)
{
    const QTextBlock hovered = doc ? doc->findBlockByNumber(block) : QTextBlock();
    if (!hovered.isValid())
        return {};
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const Item                   item   = itemOf(hovered, env);
    const QRectF                 rect   = item.kind != Kind::None ? item.rect : layout->blockBoundingRect(hovered);
    int                          from = block, to = block;
    for (QTextBlock b = hovered.previous(); b.isValid(); b = b.previous()) {
        if (!b.isVisible())
            continue;
        if (layout->blockBoundingRect(b).bottom() < rect.top() - reach)
            break;
        from = b.blockNumber();
    }
    for (QTextBlock b = hovered.next(); b.isValid(); b = b.next()) {
        if (!b.isVisible())
            continue;
        if (layout->blockBoundingRect(b).top() > rect.bottom() + reach)
            break;
        to = b.blockNumber();
    }
    return keepOut(doc, from, to, env);
}

namespace {

constexpr int kBarAir   = 6; // a bar beside a bubble: this far from it
constexpr int kBarEdge  = 2; // inside the viewport by this much
constexpr int kBarReach = 4; // a bar this far above or below the bubble is still its

qint64 coveredBy(const QRect& r, const QVector<QRect>& keepOut)
{
    qint64 area = 0;
    for (const QRect& k : keepOut) {
        const QRect i = r & k;
        if (!i.isEmpty())
            area += qint64(i.width()) * i.height();
    }
    return area;
}

} // namespace

QRect barPlace(const QSize& bar, const QRect& area, const QRect& bubble, const QVector<QRect>& keepOut, int right, int textPad, bool left)
{
    if (left) {
        // 2.3.1: your message on the right: the same search on the area mirrored (x -> area's left + right - x).
        const auto mirror = [&area](const QRect& r) { return QRect(area.left() + area.right() - r.right(), r.top(), r.width(), r.height()); };
        QVector<QRect> keep;
        keep.reserve(keepOut.size());
        for (const QRect& k : keepOut)
            keep << mirror(k);
        return mirror(barPlace(bar, area, mirror(bubble), keep, right, textPad, false));
    }
    const int w = bar.width(), h = bar.height();
    const int minX = area.left() + kBarEdge, maxX = qMax(minX, area.right() + 1 - kBarEdge - w);
    const int minY = area.top() + kBarEdge, maxY = qMax(minY, area.bottom() + 1 - kBarEdge - h);
    const int edgeX = qBound(minX, area.right() + 1 - right - w, maxX); // at the right, inset
    // As in 2.3: beside the top right corner (centred on a one-line bubble, lower than the bar), else on the
    // bubble's top edge at the right.
    const bool beside = bubble.right() + kBarAir <= edgeX;
    const int  wantX  = qBound(minX, beside ? bubble.right() + kBarAir : edgeX, maxX);
    const int  wantY  = qBound(minY, beside ? bubble.top() - qMax(0, (h - bubble.height()) / 2) : bubble.top() - h / 2, maxY);
    const QRect want(wantX, wantY, w, h);
    if (coveredBy(want, keepOut) == 0)
        return want;
    // Its own: within kBarReach above or below the bubble (and inside the viewport), at its right or over its
    // top right corner (the bar's right end at least that far right).
    const int lo     = qMax(minY, bubble.top() - kBarReach - h + 1);
    const int hi     = qMin(maxY, bubble.bottom() + kBarReach);
    const int corner = qBound(minX, bubble.right() + 1 - kBarAir - w, maxX);
    // Where to look, in this order: here; then further right, past what is in the way, and back left towards the
    // bubble's top right corner, keeping less air each round (a sliding search over these is complete: a free
    // place, if there is one, has its left edge at one of them).
    const auto sorted = [](QVector<int> xs, bool up) {
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        if (!up)
            std::reverse(xs.begin(), xs.end());
        return xs;
    };
    // (Air: clear of the text's bubble and the bar's own air first; then only the bar's air; then none.)
    const QVector<int> airs  = textPad > 0 ? QVector<int>{textPad + kBarAir, kBarAir, 0} : QVector<int>{kBarAir, 0};
    const int          first = qMin(corner, wantX); // the leftmost place looked at
    QVector<int>       xs{wantX};
    for (int level = 0; level < airs.size(); ++level) {
        const int    air = airs.at(level);
        QVector<int> at  = level == 0 ? QVector<int>{edgeX, corner} : QVector<int>{maxX};
        for (const QRect& k : keepOut)
            at << k.right() + 1 + air << k.left() - air - w; // right of it, left of it
        QVector<int> further, back;
        for (const int x : qAsConst(at)) {
            if (x > wantX && x <= maxX)
                further << x;
            else if (x < wantX && x >= first)
                back << x;
        }
        xs << sorted(further, true) << sorted(back, false);
    }
    QRect  best  = want;
    qint64 least = coveredBy(want, keepOut);
    for (const int x : qAsConst(xs)) {
        // The tops worth a look at this x: as wanted, right above or below what is in the way, the ends.
        QVector<int> ys{qBound(lo, wantY, qMax(lo, hi)), lo, hi};
        for (const QRect& k : keepOut) {
            if (k.left() > x + w - 1 || k.right() < x)
                continue;
            ys << k.top() - h << k.bottom() + 1;
        }
        int found = INT_MIN;
        for (const int y : qAsConst(ys)) {
            if (y < lo || y > hi)
                continue;
            const QRect  r(x, y, w, h);
            const qint64 c = coveredBy(r, keepOut);
            if (c == 0 && (found == INT_MIN || qAbs(y - wantY) < qAbs(found - wantY)))
                found = y;
            if (c < least) {
                least = c;
                best  = r;
            }
        }
        if (found != INT_MIN)
            return QRect(x, found, w, h);
    }
    return best;
}

QPainterPath pathOf(const Item& item, const layoutart::Tokens& t)
{
    QPainterPath path;
    const QRectF r = item.rect;
    if (r.isEmpty())
        return path;
    if (item.kind == Kind::Pill) {
        const qreal radius = qMin(r.height() / 2.0, qreal(t.bubbleRadius));
        path.addRoundedRect(r, radius, radius);
        return path;
    }
    const qreal limit = qMin(r.width(), r.height()) / 2.0;
    const qreal big   = qMin(qreal(t.bubbleRadius), limit);
    const qreal small = qMin(qreal(t.bubbleRadiusJoined), limit);
    // Small where the group's bubbles meet: on the left, on the right for yours on the right (2.3.1).
    const qreal above = item.joinAbove ? small : big;
    const qreal below = item.joinBelow ? small : big;
    const qreal tl    = item.right ? big : above;
    const qreal bl    = item.right ? big : below;
    const qreal tr    = item.right ? above : big;
    const qreal br    = item.right ? below : big;
    path.moveTo(r.left() + tl, r.top());
    path.lineTo(r.right() - tr, r.top());
    path.arcTo(QRectF(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr), 90, -90);
    path.lineTo(r.right(), r.bottom() - br);
    path.arcTo(QRectF(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
    path.lineTo(r.left() + bl, r.bottom());
    path.arcTo(QRectF(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
    path.lineTo(r.left(), r.top() + tl);
    path.arcTo(QRectF(r.left(), r.top(), 2 * tl, 2 * tl), 180, -90);
    path.closeSubpath();
    return path;
}

void paint(QPainter& p, const QVector<Item>& items, const layoutdoc::Env& env, const QPointF& offset, const State& state)
{
    if (items.isEmpty())
        return;
    const layoutart::Colors& c = env.colors;
    const layoutart::Tokens& t = env.tokens;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    for (const Item& item : items) {
        const QPainterPath path = pathOf(item, t).translated(offset);
        if (path.isEmpty())
            continue;
        const bool hovered = item.block == state.hover || item.block == state.focus;
        if (item.kind == Kind::Pill) {
            p.fillPath(path, c.pill);
            if (hovered)
                p.fillPath(path, c.bubbleHover);
            continue;
        }
        p.fillPath(path, item.own ? c.bubbleOwn : c.bubble);
        if (item.mention) {
            // A message that mentions you: the tint, and the bar along its left edge (inside the outline).
            p.fillPath(path, c.mentionTint);
            p.save();
            p.setClipPath(path, Qt::IntersectClip);
            const QRectF r = item.rect.translated(offset);
            p.fillRect(QRectF(r.left(), r.top(), 3, r.height()), c.mentionBar);
            p.restore();
        }
        if (hovered)
            p.fillPath(path, c.bubbleHover);
        const auto inset = [&item](qreal d) {
            Item in = item;
            in.rect = item.rect.adjusted(d, d, -d, -d);
            return in;
        };
        if (c.bubbleEdge.alpha() > 0) {
            // Light skins: a hairline keeps a near-white bubble apart from the white chat.
            p.strokePath(pathOf(inset(0.5), t).translated(offset), QPen(c.bubbleEdge, 1.0));
        }
        if (item.block == state.focus)
            p.strokePath(pathOf(inset(0.75), t).translated(offset), QPen(c.focusBar, 1.5));
        // 2.3.1: your message's time on its bubble (never on its text: settle() made room for it).
        if (!item.time.isEmpty())
            layoutart::paintOwnTime(p, item.time, item.timeBaseline + offset, t, c.mutedOnBubble);
    }
    p.restore();
}

} // namespace layoutbubbles
