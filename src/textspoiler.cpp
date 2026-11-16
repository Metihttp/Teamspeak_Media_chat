#include "textspoiler.h"

namespace textspoiler {

namespace {

constexpr int kOpenLength  = 9;  // "[spoiler]"
constexpr int kCloseLength = 10; // "[/spoiler]"

bool blank(const QString& text, int from, int to)
{
    for (int i = from; i < to; ++i) {
        if (!text.at(i).isSpace())
            return false;
    }
    return true;
}

} // namespace

QString openMarker()
{
    return QString::fromLatin1("[spoiler]");
}

QString closeMarker()
{
    return QString::fromLatin1("[/spoiler]");
}

QString placeholder()
{
    return QString::fromLatin1("(spoiler)");
}

QVector<Range> ranges(const QString& text, int max)
{
    QVector<Range> out;
    const QString  open  = openMarker();
    const QString  close = closeMarker();
    int            pos   = 0;
    while (out.size() < max) {
        const int o = text.indexOf(open, pos, Qt::CaseInsensitive);
        if (o < 0)
            break;
        const int c = text.indexOf(close, o + kOpenLength, Qt::CaseInsensitive);
        if (c < 0)
            break; // never closed: shown as typed
        pos = c + kCloseLength;
        if (blank(text, o + kOpenLength, c))
            continue; // nothing to hide: shown as typed
        out.append({o, o + kOpenLength, c, c + kCloseLength});
    }
    return out;
}

bool mayHold(const QString& text)
{
    return text.contains(QLatin1String("[spoiler"), Qt::CaseInsensitive);
}

QString masked(const QString& text, bool openToEnd)
{
    const QVector<Range> pairs = ranges(text);
    QString              out;
    int                  at = 0;
    for (const Range& r : pairs) {
        out += text.mid(at, r.open - at);
        out += placeholder();
        at = r.closeEnd;
    }
    if (openToEnd) {
        const int o = text.indexOf(openMarker(), at, Qt::CaseInsensitive);
        if (o >= 0 && !blank(text, o + kOpenLength, text.size())) {
            out += text.mid(at, o - at);
            out += placeholder();
            return out;
        }
    }
    out += text.mid(at);
    return out;
}

QString withoutHidden(const QString& bbcode)
{
    const QVector<Range> pairs = ranges(bbcode);
    if (pairs.isEmpty())
        return bbcode;
    QString out;
    int     at = 0;
    for (const Range& r : pairs) {
        out += bbcode.mid(at, r.open - at);
        at = r.closeEnd;
        // A "[URL]" opened inside and closed right after the pair (the client wrapped an address that ran into the
        // closing marker): its closing tag goes too.
        const QString content = bbcode.mid(r.contentStart, r.contentEnd - r.contentStart);
        const int     opened  = content.count(QLatin1String("[url"), Qt::CaseInsensitive);
        const int     closed  = content.count(QLatin1String("[/url]"), Qt::CaseInsensitive);
        if (opened > closed && bbcode.midRef(at, 6).compare(QLatin1String("[/url]"), Qt::CaseInsensitive) == 0)
            at += 6;
    }
    out += bbcode.mid(at);
    return out;
}

} // namespace textspoiler
