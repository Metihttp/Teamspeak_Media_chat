#include "emojisegment.h"

#include <algorithm>

#include "emojidata.h"

namespace emoji {

namespace {

constexpr ushort kZwj    = 0x200D;
constexpr ushort kVs16   = 0xFE0F;
constexpr ushort kVs15   = 0xFE0E;
constexpr ushort kKeycap = 0x20E3;

uint codePointAt(const QString& text, int i, int* units)
{
    const QChar c = text.at(i);
    if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
        *units = 2;
        return QChar::surrogateToUcs4(c, text.at(i + 1));
    }
    *units = 1;
    return c.unicode();
}

bool isRegional(uint cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

bool isModifier(uint cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

bool isTag(uint cp)
{
    return cp >= 0xE0020 && cp <= 0xE007E;
}

// Text-default characters people type meaning the emoji (shown as pictures even without U+FE0F).
bool meantAsEmoji(uint cp)
{
    switch (cp) {
    case 0x2764: // ❤
    case 0x263A: // ☺
    case 0x2639: // ☹
    case 0x270C: // ✌
    case 0x261D: // ☝
    case 0x270D: // ✍
    case 0x2620: // ☠
    case 0x2763: // ❣
    case 0x2665: // ♥
    case 0x2600: // ☀
    case 0x2601: // ☁
    case 0x2744: // ❄
    case 0x2708: // ✈
    case 0x26A0: // ⚠
        return true;
    default:
        return false;
    }
}

// One emoji element at 'at' (a base with its selector, modifier, keycap or tags; or a flag pair): its
// end, and the ends a shorter match could use.
int element(const QString& text, int at, QVector<int>* ends, bool* textSelector)
{
    const int  n     = text.size();
    int        units = 0;
    const uint base  = codePointAt(text, at, &units);
    int        q     = at + units;
    if (isRegional(base)) {
        int second = 0;
        if (q < n && isRegional(codePointAt(text, q, &second)))
            q += second;
        ends->append(q);
        return q;
    }
    if (q < n && (text.at(q).unicode() == kVs16 || text.at(q).unicode() == kVs15)) {
        if (textSelector)
            *textSelector = text.at(q).unicode() == kVs15;
        ++q;
    }
    ends->append(q);
    if (q < n) {
        int        u  = 0;
        const uint cp = codePointAt(text, q, &u);
        if (isModifier(cp)) {
            q += u;
            ends->append(q);
        }
    }
    if (q < n && text.at(q).unicode() == kKeycap) {
        ++q;
        ends->append(q);
    }
    int  t    = q;
    bool tags = false;
    int  guard = 0;
    while (t < n && guard++ < 40) {
        int        u  = 0;
        const uint cp = codePointAt(text, t, &u);
        if (!isTag(cp))
            break;
        t += u;
        tags = true;
    }
    if (tags && t < n) {
        int u = 0;
        if (codePointAt(text, t, &u) == 0xE007F) {
            q = t + u;
            ends->append(q);
        }
    }
    return q;
}

} // namespace

QVector<Match> findEmoji(const QString& text, int maxMatches)
{
    QVector<Match> out;
    const int      n       = text.size();
    const int      longest = maxKeyUnits() + 16; // + selectors
    int            i       = 0;
    while (i < n && out.size() < maxMatches) {
        const ushort u = text.at(i).unicode();
        if (u < 0xA9) {
            // Only keycaps start in ASCII ("1️⃣", "#⃣").
            const bool keycapBase = u == '#' || u == '*' || (u >= '0' && u <= '9');
            const bool follows    = i + 1 < n && (text.at(i + 1).unicode() == kKeycap || text.at(i + 1).unicode() == kVs16);
            if (!keycapBase || !follows) {
                ++i;
                continue;
            }
        }
        int        units = 0;
        const uint cp    = codePointAt(text, i, &units);
        if (!mayStart(cp)) {
            i += units;
            continue;
        }
        QVector<int> ends;
        bool         textSelector = false;
        int          p            = element(text, i, &ends, &textSelector);
        const int    firstEnd     = ends.isEmpty() ? p : ends.first();
        // ZWJ chains (bounded by the longest table emoji).
        while (p + 1 < n && text.at(p).unicode() == kZwj && p - i < longest) {
            int        u2   = 0;
            const uint next = codePointAt(text, p + 1, &u2);
            if (next < 0x80)
                break;
            p = element(text, p + 1, &ends, nullptr);
        }
        std::sort(ends.begin(), ends.end());
        ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
        int matchedEnd = -1;
        int matchedId  = -1;
        for (int k = ends.size() - 1; k >= 0; --k) {
            const int e = ends.at(k);
            if (e - i > longest)
                continue;
            const int id = findKey(lookupKey(text.mid(i, e - i)));
            if (id >= 0) {
                matchedEnd = e;
                matchedId  = id;
                break;
            }
        }
        // U+FE0E asks for the text form: left alone.
        if (matchedId >= 0 && !(textSelector && matchedEnd == firstEnd)) {
            Match m;
            m.start    = i;
            m.length   = matchedEnd - i;
            m.id       = matchedId;
            m.selector = text.midRef(i, m.length).contains(QChar(kVs16));
            out.append(m);
            i = matchedEnd;
        } else {
            i += matchedId >= 0 ? matchedEnd - i : units;
        }
    }
    return out;
}

bool showsAsPicture(const Match& match)
{
    if (!isValid(match.id))
        return false;
    if (!isTextDefault(match.id) || match.selector)
        return true;
    const QString t = text(match.id);
    int           units = 0;
    return !t.isEmpty() && meantAsEmoji(codePointAt(t, 0, &units));
}

int pictureOnlyCount(const QString& text, int maxCount)
{
    const QVector<Match> matches = findEmoji(text, qMax(0, maxCount) + 1);
    int                  count   = 0;
    int                  next    = 0;
    for (int i = 0; i < text.size();) {
        if (next < matches.size() && matches.at(next).start == i) {
            if (!showsAsPicture(matches.at(next)))
                return -1;
            ++count;
            i += matches.at(next).length;
            ++next;
            if (count > maxCount)
                return count;
            continue;
        }
        const QChar ch = text.at(i);
        if (ch.isSpace() || ch.unicode() == 0x200B || ch.unicode() == kZwj || ch.unicode() == kVs16 || ch.unicode() == QChar::LineSeparator
            || ch.unicode() == QChar::ParagraphSeparator) {
            ++i;
            continue;
        }
        return -1;
    }
    return count;
}

} // namespace emoji
