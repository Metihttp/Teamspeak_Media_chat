#include "textformat.h"

#include <QVector>

#include <algorithm>

namespace textformat {

namespace {

constexpr Format kAll[] = {Format::Bold, Format::Italic, Format::Underline, Format::Spoiler};

struct TagPair {
    int open     = 0; // the opening tag
    int openEnd  = 0;
    int close    = 0; // the closing tag
    int closeEnd = 0;
};

// Every pair of f's tags in text (any case). B/I/U nest like TeamSpeak reads them (a closing tag closes the latest
// open one); spoilers don't nest (an opening marker pairs with the first closing one after it, textspoiler.h).
// Empty pairs count (the cursor may sit between them).
QVector<TagPair> pairsOf(const QString& text, Format f)
{
    QVector<TagPair> out;
    const QString    open  = openTag(f);
    const QString    close = closeTag(f);
    if (f == Format::Spoiler) {
        int pos = 0;
        for (;;) {
            const int o = text.indexOf(open, pos, Qt::CaseInsensitive);
            if (o < 0)
                break;
            const int c = text.indexOf(close, o + open.size(), Qt::CaseInsensitive);
            if (c < 0)
                break;
            out.append({o, o + open.size(), c, c + close.size()});
            pos = c + close.size();
        }
        return out;
    }
    QVector<int> stack;
    for (int i = text.indexOf(QLatin1Char('[')); i >= 0; i = text.indexOf(QLatin1Char('['), i + 1)) {
        if (text.midRef(i, open.size()).compare(open, Qt::CaseInsensitive) == 0) {
            stack.append(i);
        } else if (text.midRef(i, close.size()).compare(close, Qt::CaseInsensitive) == 0 && !stack.isEmpty()) {
            const int o = stack.takeLast();
            out.append({o, o + open.size(), i, i + close.size()});
        }
    }
    std::sort(out.begin(), out.end(), [](const TagPair& a, const TagPair& b) { return a.open < b.open; });
    return out;
}

void trim(const QString& text, int* s, int* e)
{
    while (*s < *e && text.at(*s).isSpace())
        ++*s;
    while (*e > *s && text.at(*e - 1).isSpace())
        --*e;
}

bool clampSelection(const QString& text, int* s, int* e)
{
    if (*s > *e)
        std::swap(*s, *e);
    *s = qBound(0, *s, text.size());
    *e = qBound(0, *e, text.size());
    return true;
}

// The pair of f that the trimmed selection [s, e) is: its tags at both ends of the selection, or right around it,
// with only other formats' pairs that hold exactly the same text in between ([I][B]x[/B][/I] is bold and italic,
// whichever way it is selected). Spoiler: also a spoiler whose text holds the selection. Index into pairs, or -1.
int appliedPair(const QString& text, int s, int e, Format f, const QVector<TagPair>& pairs)
{
    if (s >= e)
        return -1;
    QVector<TagPair> all[4];
    for (int k = 0; k < 4; ++k)
        all[k] = kAll[k] == f ? pairs : pairsOf(text, kAll[k]);
    const auto exact = [&](int from, int to, bool inner, Format* which, TagPair* pair) {
        for (int k = 0; k < 4; ++k) {
            for (const TagPair& p : qAsConst(all[k])) {
                if ((inner && p.open == from && p.closeEnd == to) || (!inner && p.openEnd == from && p.close == to)) {
                    *which = kAll[k];
                    *pair  = p;
                    return true;
                }
            }
        }
        return false;
    };
    const auto indexOf = [&pairs](const TagPair& p) {
        for (int i = 0; i < pairs.size(); ++i) {
            if (pairs.at(i).open == p.open && pairs.at(i).close == p.close)
                return i;
        }
        return -1;
    };
    // Tags inside the selection, at its ends: inwards.
    for (int from = s, to = e; from < to;) {
        Format  which;
        TagPair p;
        if (!exact(from, to, true, &which, &p))
            break;
        if (which == f)
            return indexOf(p);
        from = p.openEnd;
        to   = p.close;
    }
    // Tags right around it: outwards.
    for (int from = s, to = e;;) {
        Format  which;
        TagPair p;
        if (!exact(from, to, false, &which, &p))
            break;
        if (which == f)
            return indexOf(p);
        from = p.open;
        to   = p.closeEnd;
    }
    if (f == Format::Spoiler) {
        for (int i = 0; i < pairs.size(); ++i) {
            if (pairs.at(i).openEnd <= s && e <= pairs.at(i).close)
                return i;
        }
    }
    return -1;
}

// Where a position of text lands once pair's tags are taken out.
int mapPast(int p, const TagPair& pair)
{
    const int openLength = pair.openEnd - pair.open;
    if (p <= pair.open)
        return p;
    if (p <= pair.openEnd)
        return pair.open;
    if (p <= pair.close)
        return p - openLength;
    if (p <= pair.closeEnd)
        return pair.close - openLength;
    return p - openLength - (pair.closeEnd - pair.close);
}

bool isAddressStart(const QString& word)
{
    return word.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || word.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
           || word.startsWith(QLatin1String("www."), Qt::CaseInsensitive);
}

} // namespace

QString openTag(Format f)
{
    switch (f) {
    case Format::Bold:
        return QString::fromLatin1("[B]");
    case Format::Italic:
        return QString::fromLatin1("[I]");
    case Format::Underline:
        return QString::fromLatin1("[U]");
    case Format::Spoiler:
        break;
    }
    return QString::fromLatin1("[spoiler]");
}

QString closeTag(Format f)
{
    switch (f) {
    case Format::Bold:
        return QString::fromLatin1("[/B]");
    case Format::Italic:
        return QString::fromLatin1("[/I]");
    case Format::Underline:
        return QString::fromLatin1("[/U]");
    case Format::Spoiler:
        break;
    }
    return QString::fromLatin1("[/spoiler]");
}

bool isApplied(const QString& text, int selStart, int selEnd, Format f)
{
    int s = selStart;
    int e = selEnd;
    clampSelection(text, &s, &e);
    trim(text, &s, &e);
    return appliedPair(text, s, e, f, pairsOf(text, f)) >= 0;
}

Edit apply(const QString& text, int selStart, int selEnd, Format f)
{
    Edit out;
    int  s = selStart;
    int  e = selEnd;
    clampSelection(text, &s, &e);
    trim(text, &s, &e);
    const QString open  = openTag(f);
    const QString close = closeTag(f);

    // Nothing selected: an empty pair, the cursor between its tags.
    if (s >= e) {
        out.from        = s;
        out.to          = s;
        out.replacement = open + close;
        out.selStart = out.selEnd = s + open.size();
        out.ok                    = true;
        return out;
    }

    // Toggle off.
    const QVector<TagPair> pairs = pairsOf(text, f);
    const int              found = appliedPair(text, s, e, f, pairs);
    if (found >= 0) {
        const TagPair& p = pairs.at(found);
        out.from         = p.open;
        out.to           = p.closeEnd;
        out.replacement  = text.mid(p.openEnd, p.close - p.openEnd);
        out.selStart     = mapPast(s, p);
        out.selEnd       = mapPast(e, p);
        out.ok           = true;
        return out;
    }

    // Grow over every pair that the selection cuts (one of its tags inside, the other outside).
    QVector<TagPair> all;
    for (const Format g : kAll)
        all += pairsOf(text, g);
    for (bool grew = true; grew;) {
        grew = false;
        for (const TagPair& p : qAsConst(all)) {
            const bool meets    = s < p.closeEnd && e > p.open;
            const bool holds    = s <= p.open && e >= p.closeEnd;
            const bool inside   = s >= p.openEnd && e <= p.close;
            if (meets && !holds && !inside) {
                s    = qMin(s, p.open);
                e    = qMax(e, p.closeEnd);
                grew = true;
            }
        }
    }

    // The same format's tags inside go (no nesting): its pairs; for Spoiler every marker (a stray closing marker
    // inside would end the new spoiler early).
    QVector<QPair<int, int>> drop; // [from, to) of the text
    if (f == Format::Spoiler) {
        for (const Format g : {Format::Spoiler}) {
            for (const QString& tag : {openTag(g), closeTag(g)}) {
                for (int i = text.indexOf(tag, s, Qt::CaseInsensitive); i >= 0 && i + tag.size() <= e; i = text.indexOf(tag, i + tag.size(), Qt::CaseInsensitive))
                    drop.append({i, i + tag.size()});
            }
        }
    } else {
        for (const TagPair& p : pairs) {
            if (p.open >= s && p.closeEnd <= e) {
                drop.append({p.open, p.openEnd});
                drop.append({p.close, p.closeEnd});
            }
        }
    }
    std::sort(drop.begin(), drop.end());
    QString inner;
    int     at = s;
    for (const auto& d : qAsConst(drop)) {
        if (d.first < at)
            continue;
        inner += text.midRef(at, d.first - at);
        at = d.second;
    }
    inner += text.midRef(at, e - at);

    // A bare web address at the end, right before the closing tag: in [URL] first. For a spoiler whatever follows the
    // selection (review fix): TeamSpeak's own address wrapping would otherwise take "[/spoiler]" and the text after it
    // ("a.png[/spoiler],") into one link, and that link's visible part would lead to the hidden address.
    if (f == Format::Spoiler || e == text.size() || text.at(e).isSpace()) {
        int wordStart = inner.size();
        while (wordStart > 0 && !inner.at(wordStart - 1).isSpace())
            --wordStart;
        const QString word = inner.mid(wordStart);
        if (isAddressStart(word) && !word.contains(QLatin1Char('[')) && !word.contains(QLatin1Char(']')))
            inner = inner.left(wordStart) + QLatin1String("[URL]") + word + QLatin1String("[/URL]");
    }

    out.from        = s;
    out.to          = e;
    out.replacement = open + inner + close;
    out.selStart    = s + open.size();
    out.selEnd      = out.selStart + inner.size();
    out.ok          = true;
    return out;
}

QString applied(const QString& text, const Edit& edit)
{
    if (!edit.valid())
        return text;
    return text.left(edit.from) + edit.replacement + text.mid(edit.to);
}

} // namespace textformat
