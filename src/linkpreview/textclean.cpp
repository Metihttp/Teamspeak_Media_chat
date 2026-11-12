#include "textclean.h"

#include <QTextBoundaryFinder>
#include <QVector>

namespace linkpreview::textclean {

namespace {

constexpr uint kZwnj  = 0x200C;
constexpr uint kZwj   = 0x200D;
constexpr uint kSpace = 0x20;

bool isJoiner(uint u)
{
    return u == kZwnj || u == kZwj;
}

bool isVariationSelector(uint u)
{
    return u >= 0xFE00 && u <= 0xFE0F;
}

bool isMark(uint u)
{
    const QChar::Category c = QChar::category(u);
    return c == QChar::Mark_NonSpacing || c == QChar::Mark_Enclosing;
}

// A character that shows something: a letter, mark, number, symbol or punctuation.
bool isVisible(uint u)
{
    switch (QChar::category(u)) {
    case QChar::Letter_Uppercase:
    case QChar::Letter_Lowercase:
    case QChar::Letter_Titlecase:
    case QChar::Letter_Modifier:
    case QChar::Letter_Other:
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Number_DecimalDigit:
    case QChar::Number_Letter:
    case QChar::Number_Other:
    case QChar::Symbol_Math:
    case QChar::Symbol_Currency:
    case QChar::Symbol_Modifier:
    case QChar::Symbol_Other:
    case QChar::Punctuation_Connector:
    case QChar::Punctuation_Dash:
    case QChar::Punctuation_Open:
    case QChar::Punctuation_Close:
    case QChar::Punctuation_InitialQuote:
    case QChar::Punctuation_FinalQuote:
    case QChar::Punctuation_Other:
        return true;
    default:
        return false;
    }
}

bool isWhite(uint u)
{
    return QChar::isSpace(u) || u == 0x2028 || u == 0x2029 || u == '\t' || u == '\n' || u == '\r' || u == 0x0B || u == 0x0C || u == 0x85;
}

// Rule 2: characters that never stay.
bool isRemoved(uint u)
{
    if (isJoiner(u))
        return false;
    const QChar::Category c = QChar::category(u);
    if (c == QChar::Other_Control || c == QChar::Other_Format || c == QChar::Other_Surrogate || c == QChar::Other_PrivateUse)
        return true;
    if (u == 0x061C || u == 0x200B || u == 0x200E || u == 0x200F || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x2064)
        || (u >= 0x2066 && u <= 0x206F) || u == 0xFEFF || (u >= 0xFFF9 && u <= 0xFFFD))
        return true;
    // Invisible fillers (Hangul fillers, the braille blank, the combining grapheme joiner, Khmer inherent vowels, the
    // Mongolian vowel separator): letters or symbols by category, but they show nothing.
    if (u == 0x115F || u == 0x1160 || u == 0x3164 || u == 0xFFA0 || u == 0x2800 || u == 0x034F || u == 0x17B4 || u == 0x17B5 || u == 0x180E)
        return true;
    if (u >= 0xE0000 && u <= 0xE007F) // tag characters
        return true;
    if (u >= 0xE0100 && u <= 0xE01EF) // variation selectors supplement
        return true;
    if ((u >= 0xFDD0 && u <= 0xFDEF) || (u & 0xFFFE) == 0xFFFE) // noncharacters
        return true;
    if ((u >= 0xE000 && u <= 0xF8FF) || u >= 0xF0000) // private use, planes 15-16
        return true;
    return false;
}

QVector<uint> codePoints(const QString& text)
{
    QVector<uint> out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isHighSurrogate()) {
            if (i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
                out.append(QChar::surrogateToUcs4(ch, text.at(i + 1)));
                ++i;
            }
            continue; // a lone high surrogate
        }
        if (ch.isLowSurrogate())
            continue; // a lone low surrogate
        out.append(ch.unicode());
    }
    return out;
}

// Rules 1-5 (see textclean.h). Idempotent: cleanPass(cleanPass(x)) == cleanPass(x).
QString cleanPass(const QString& text)
{
    const QVector<uint> in = codePoints(text);

    // Pass 1: white space collapsed, rule 2.
    QVector<uint> a;
    a.reserve(in.size());
    for (const uint u : in) {
        if (isWhite(u)) {
            if (!a.isEmpty() && a.last() != kSpace)
                a.append(kSpace);
            continue;
        }
        if (isRemoved(u))
            continue;
        a.append(u);
    }

    // Pass 2: rules 4 and 5 (variation selectors, at most two marks per base). Joiners don't start a new base.
    QVector<uint> b;
    b.reserve(a.size());
    int marks = 0;
    for (const uint u : a) {
        if (isVariationSelector(u)) {
            const uint prev = b.isEmpty() ? 0 : b.last();
            if (prev == 0 || !isVisible(prev) || isVariationSelector(prev) || isJoiner(prev))
                continue;
            if (++marks > 2)
                continue;
            b.append(u);
            continue;
        }
        if (isMark(u)) {
            if (++marks > 2)
                continue;
            b.append(u);
            continue;
        }
        if (!isJoiner(u))
            marks = 0;
        b.append(u);
    }

    // Pass 3: rule 3 (joiners only between two visible characters), white space collapsed again.
    QVector<uint> c;
    c.reserve(b.size());
    for (int i = 0; i < b.size(); ++i) {
        const uint u = b.at(i);
        if (isJoiner(u)) {
            const uint prev = c.isEmpty() ? 0 : c.last();
            const uint next = i + 1 < b.size() ? b.at(i + 1) : 0;
            if (prev == 0 || next == 0 || !isVisible(prev) || !isVisible(next))
                continue;
            c.append(u);
            continue;
        }
        if (u == kSpace && (c.isEmpty() || c.last() == kSpace))
            continue;
        c.append(u);
    }
    while (!c.isEmpty() && c.last() == kSpace)
        c.removeLast();
    return QString::fromUcs4(c.constData(), c.size());
}

bool isEmojiModifier(uint u)
{
    return u >= 0x1F3FB && u <= 0x1F3FF;
}

uint codePointAt(const QString& s, int pos)
{
    const QChar ch = s.at(pos);
    if (ch.isHighSurrogate() && pos + 1 < s.size() && s.at(pos + 1).isLowSurrogate())
        return QChar::surrogateToUcs4(ch, s.at(pos + 1));
    return ch.unicode();
}

// Grapheme boundaries of s where a cut is safe: Qt's extended grapheme clusters, and never next to a ZWJ (emoji
// sequences), before a mark, a variation selector or an emoji modifier, or inside a surrogate pair.
QVector<int> safeBoundaries(const QString& s)
{
    QVector<int>        out;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, s);
    finder.toStart();
    for (int p = 0; p >= 0 && p <= s.size(); p = finder.toNextBoundary()) {
        bool ok = true;
        if (p > 0 && p < s.size()) {
            const uint at = codePointAt(s, p);
            if (s.at(p).isLowSurrogate() || s.at(p - 1).unicode() == kZwj || at == kZwj || isMark(at) || isVariationSelector(at)
                || isEmojiModifier(at) || QChar::category(at) == QChar::Mark_SpacingCombining)
                ok = false;
        }
        if (ok)
            out.append(p);
        if (p == s.size())
            break;
    }
    if (out.isEmpty() || out.last() != s.size())
        out.append(s.size());
    return out;
}

// What a cleaner looks at: a hostile value is cut long before it is cleaned.
QString bounded(const QString& text, int limit)
{
    const int keep = qMax(8192, 16 * qMax(0, limit)) + 64;
    return text.size() > keep ? text.left(keep) : text;
}

} // namespace

int utf8Bytes(const QString& text)
{
    int bytes = 0;
    for (int i = 0; i < text.size(); ++i) {
        const ushort u = text.at(i).unicode();
        if (u < 0x80)
            bytes += 1;
        else if (u < 0x800)
            bytes += 2;
        else if (QChar::isHighSurrogate(u) && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            bytes += 4;
            ++i;
        } else
            bytes += 3;
    }
    return bytes;
}

QString clean(const QString& text, int maxChars)
{
    if (maxChars <= 0)
        return {};
    const QString s = cleanPass(bounded(text, maxChars));
    if (s.size() <= maxChars)
        return s;
    // Cut: the last safe boundary that leaves room for the ellipsis.
    const QVector<int> cuts = safeBoundaries(s);
    int                at   = 0;
    for (const int p : cuts) {
        if (p > maxChars - 1)
            break;
        at = p;
    }
    // The second pass never makes a text longer: it only drops a joiner or white space left at the new end.
    return cleanPass(s.left(at)) + QChar(0x2026);
}

QString cleanToBytes(const QString& text, int maxBytes)
{
    if (maxBytes <= 0)
        return {};
    const QString s = cleanPass(bounded(text, maxBytes));
    if (utf8Bytes(s) <= maxBytes)
        return s;
    // UTF-8 bytes before every UTF-16 position.
    QVector<int> before(s.size() + 1, 0);
    for (int i = 0; i < s.size(); ++i) {
        const ushort u = s.at(i).unicode();
        const int    n = u < 0x80 ? 1 : u < 0x800 ? 2 : QChar::isLowSurrogate(u) ? 1 : 3; // a pair: 3 + 1 = 4
        before[i + 1]  = before[i] + n;
    }
    const QVector<int> cuts = safeBoundaries(s);
    int                at   = 0;
    for (const int p : cuts) {
        if (before[p] > maxBytes)
            break;
        at = p;
    }
    return cleanPass(s.left(at));
}

} // namespace linkpreview::textclean
