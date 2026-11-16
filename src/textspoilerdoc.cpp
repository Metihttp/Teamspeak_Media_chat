#include "textspoilerdoc.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QVariantList>

#include <algorithm>

#include "emojiformat.h"
#include "layoutformat.h"
#include "replydoc.h"
#include "textspoiler.h"

namespace textspoilerdoc {

namespace {

// Properties of TS Media's modules in chat documents (0x7453-0x74ff); a fragment with one of them isn't TeamSpeak's
// text, except the chat layout's recolouring (its kApplied and kOrigChar on TeamSpeak's own text).
bool hasOurProperty(const QTextFormat& format)
{
    const QMap<int, QVariant> properties = format.properties();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const int id = it.key();
        if (id >= QTextFormat::UserProperty + 0x7450 && id <= QTextFormat::UserProperty + 0x74ff && id != layoutformat::kApplied
            && id != layoutformat::kOrigChar)
            return true;
    }
    return false;
}

bool isFileLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

bool isTeamSpeakEmoticon(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.toImageFormat().name().contains(QLatin1String("emoticon"), Qt::CaseInsensitive) && !hasOurProperty(format);
}

// A fragment of TeamSpeak's text (a spoiler may hold it): text, one of TeamSpeak's emoticons, an HD emoji.
bool isTeamSpeakText(const QTextCharFormat& format)
{
    if (emojiformat::isHd(format))
        return true;
    if (format.isImageFormat())
        return isTeamSpeakEmoticon(format);
    if (format.objectType() != QTextFormat::NoObject)
        return false;
    return !isFileLink(format) && !hasOurProperty(format);
}

struct Piece {
    int             start = 0;
    int             end   = 0;
    QString         text;
    QTextCharFormat format;
};

QVector<Piece> piecesOf(const QTextBlock& block)
{
    QVector<Piece> out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid())
            out.append({f.position(), f.position() + f.length(), f.text(), f.charFormat()});
    }
    return out;
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

// [from, to) of pieces as the runs TeamSpeak showed (kRuns) and what parseBlock reads for them (kIdentity).
void runsOf(const QVector<Piece>& pieces, int from, int to, QVariantList* runs, QString* identity)
{
    for (const Piece& p : pieces) {
        const int a = qMax(from, p.start);
        const int b = qMin(to, p.end);
        if (a >= b)
            continue;
        const QString text = p.text.mid(a - p.start, b - a);
        *identity += replydoc::pieceText(text, p.format);
        QTextCharFormat format = p.format;
        if (!emojiformat::isHd(format) && format.hasProperty(layoutformat::kOrigChar)) // the chat layout's recolouring: before it
            format = qvariant_cast<QTextFormat>(format.property(layoutformat::kOrigChar)).toCharFormat();
        const QPair<QString, QTextFormat> received = replydoc::receivedRun(text, format);
        *runs << received.first;
        *runs << static_cast<QVariant>(received.second);
    }
}

QTextCharFormat objectFormat(Kind kind, int id, int piece, const QVariantList& runs, const QString& identity)
{
    QTextCharFormat f;
    f.setObjectType(kObjectType);
    f.setProperty(kKind, static_cast<int>(kind));
    f.setProperty(kId, id);
    f.setProperty(kPiece, piece);
    f.setProperty(kRuns, runs);
    f.setProperty(kIdentity, identity);
    return f;
}

QColor tint()
{
    return QColor(128, 128, 128, 48); // theme-neutral: light on dark, dark on light
}

// A revealed fragment's format: the text's own with the tint, the way back in kBefore.
QTextCharFormat tinted(const QTextCharFormat& before, int id)
{
    QTextCharFormat f = before;
    f.setBackground(tint());
    f.setProperty(kKind, static_cast<int>(Revealed));
    f.setProperty(kId, id);
    f.setProperty(kBefore, static_cast<QVariant>(QTextFormat(before)));
    return f;
}

// A link's space in a hidden spoiler: no link (it would show where the link goes and get a preview).
QTextCharFormat unlinked(const QTextCharFormat& before, int id)
{
    QTextCharFormat f = before;
    f.setAnchor(false);
    f.clearProperty(QTextFormat::AnchorHref);
    f.clearProperty(QTextFormat::AnchorName);
    f.clearProperty(QTextFormat::TextToolTip);
    f.setFontUnderline(false);
    f.setProperty(kKind, static_cast<int>(Space));
    f.setProperty(kId, id);
    f.setProperty(kBefore, static_cast<QVariant>(QTextFormat(before)));
    return f;
}

// TeamSpeak's own link to a web address (not a person's link, not a TS Media file link), not one of ours.
bool isWebAnchor(const QTextCharFormat& f)
{
    if (!f.isAnchor() || f.isImageFormat() || isObject(f) || kindOf(f) != None)
        return false;
    const QString href = f.anchorHref();
    // (A person's link starts "client:"; TeamSpeak's header is read only through replydoc, tst_chatlayout's rule.)
    return !href.isEmpty() && !href.startsWith(QLatin1String("client:"), Qt::CaseInsensitive) && !href.contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

// Review fix: the parts outside a pair [open, closeEnd) of a link that runs across one of its markers. TeamSpeak's own
// address wrapping can take "[/spoiler]" and what follows into one link ("[URL]https://x/a.png[/spoiler],[/URL]"), or an
// address can run into "[spoiler]": that link's visible parts would still lead to the hidden address (its tool tip, a
// click, a link preview loading it by itself). [from, to) ranges in the document.
QVector<QPair<int, int>> linkAcrossMarkers(const QVector<Piece>& pieces, int open, int closeEnd)
{
    QVector<QPair<int, int>> out;
    const auto pieceAt = [&pieces](int position) {
        for (int k = 0; k < pieces.size(); ++k) {
            if (pieces.at(k).start <= position && position < pieces.at(k).end)
                return k;
        }
        return -1;
    };
    // After the closing marker: the link its last character belongs to, on to that link's end.
    int k = pieceAt(closeEnd - 1);
    if (k >= 0 && pieces.at(k).format.isAnchor() && !pieces.at(k).format.anchorHref().isEmpty()) {
        const QString href = pieces.at(k).format.anchorHref();
        int           at   = closeEnd;
        for (int j = k; j < pieces.size(); ++j) {
            const Piece& p = pieces.at(j);
            if (p.end <= at)
                continue;
            if (p.start > at || !isWebAnchor(p.format) || p.format.anchorHref() != href)
                break;
            out.append({at, p.end});
            at = p.end;
        }
    }
    // Before the opening marker: the link its first character belongs to, back to that link's start.
    k = pieceAt(open);
    if (k >= 0 && pieces.at(k).format.isAnchor() && !pieces.at(k).format.anchorHref().isEmpty()) {
        const QString href = pieces.at(k).format.anchorHref();
        int           at   = open;
        for (int j = k; j >= 0; --j) {
            const Piece& p = pieces.at(j);
            if (p.start >= at)
                continue;
            if (p.end < at || !isWebAnchor(p.format) || p.format.anchorHref() != href)
                break;
            out.append({p.start, at});
            at = p.start;
        }
    }
    return out;
}

void insertRuns(QTextCursor& c, const QVariantList& runs, int tintId)
{
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QTextFormat format = qvariant_cast<QTextFormat>(runs.at(i + 1));
        const QString     text   = runs.at(i).toString();
        if (format.isImageFormat()) {
            const int count = qBound(1, static_cast<int>(text.count(QChar::ObjectReplacementCharacter)), 64);
            for (int k = 0; k < count; ++k)
                c.insertImage(format.toImageFormat());
            continue;
        }
        const QTextCharFormat cf = format.toCharFormat();
        c.insertText(text, tintId >= 0 ? tinted(cf, tintId) : cf);
    }
}

// Our fragments in [from, to) of the document, last first: objects (one per character), our text fragments, and HD
// emoji whose kept format is one of our text fragments' (ChatEmoji drew them in revealed text).
struct Ours {
    int             position = 0;
    int             length   = 1;
    QTextCharFormat format;
    bool            object = false;
    bool            inner  = false; // an HD emoji with ours inside
};

QVector<Ours> oursIn(QTextDocument* doc, int from, int to)
{
    QVector<Ours> out;
    for (QTextBlock b = doc->findBlock(qMax(0, from)); b.isValid() && b.position() < to; b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || f.position() + f.length() <= from || f.position() >= to)
                continue;
            const QTextCharFormat cf = f.charFormat();
            if (isObject(cf)) {
                for (int p = f.position(); p < f.position() + f.length(); ++p)
                    out.append({p, 1, cf, true, false});
            } else if (kindOf(cf) == Revealed || kindOf(cf) == Space) {
                out.append({f.position(), f.length(), cf, false, false});
            } else if (emojiformat::isHd(cf)) {
                const int inner = kindOf(emojiformat::originalFormat(cf));
                if (inner == Revealed || inner == Space)
                    out.append({f.position(), f.length(), cf, false, true});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Ours& a, const Ours& b) { return a.position > b.position; });
    return out;
}

// One of ours back as TeamSpeak had it (the cursor's edit block is the caller's).
void giveBack(QTextDocument* doc, const Ours& o)
{
    QTextCursor c(doc);
    c.setPosition(o.position);
    c.setPosition(o.position + o.length, QTextCursor::KeepAnchor);
    if (o.object) {
        c.removeSelectedText();
        insertRuns(c, o.format.property(kRuns).toList(), -1);
    } else if (o.inner) {
        QTextCharFormat outer = o.format;
        const QTextFormat inner = emojiformat::originalFormat(o.format).property(kBefore).value<QTextFormat>();
        outer.setProperty(emojiformat::kOriginalFormat, static_cast<QVariant>(inner));
        c.setCharFormat(outer);
    } else {
        c.setCharFormat(qvariant_cast<QTextFormat>(o.format.property(kBefore)).toCharFormat());
    }
}

QFont fontOf(const QTextDocument* doc, const QTextFormat& format)
{
    return format.toCharFormat().font().resolve(doc ? doc->defaultFont() : QFont());
}

} // namespace

bool isObject(const QTextFormat& format)
{
    return format.objectType() == kObjectType;
}

int kindOf(const QTextFormat& format)
{
    return format.hasProperty(kKind) ? format.intProperty(kKind) : None;
}

QString identityText(const QTextFormat& format)
{
    return format.stringProperty(kIdentity);
}

QVector<Pair> rawPairs(const QTextBlock& block, int from)
{
    QVector<Pair> out;
    if (!block.isValid())
        return out;
    QString segment;
    int     segmentStart = -1;
    auto    flush        = [&] {
        if (!segment.isEmpty() && out.size() < textspoiler::kMaxPairs) {
            for (const textspoiler::Range& r : textspoiler::ranges(segment, textspoiler::kMaxPairs - out.size()))
                out.append({segmentStart + r.open, segmentStart + r.contentStart, segmentStart + r.contentEnd, segmentStart + r.closeEnd});
        }
        segment.clear();
        segmentStart = -1;
    };
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const int start = qMax(f.position(), from);
        const int end   = f.position() + f.length();
        if (end <= start)
            continue;
        if (!isTeamSpeakText(f.charFormat())) {
            flush();
            continue;
        }
        if (segmentStart >= 0 && segmentStart + segment.size() != start)
            flush();
        if (segmentStart < 0)
            segmentStart = start;
        segment += f.text().mid(start - f.position());
    }
    flush();
    return out;
}

int hide(QTextDocument* doc, const QVector<Pair>& pairs, const QVector<QString>& keys, const QVector<bool>& revealed, int* serial)
{
    if (!doc || pairs.isEmpty() || !serial)
        return 0;
    int         done = 0;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    for (int i = pairs.size() - 1; i >= 0; --i) {
        const Pair&      p     = pairs.at(i);
        const QTextBlock block = doc->findBlock(p.open);
        if (!block.isValid() || p.closeEnd > block.position() + block.length() - 1 || p.open >= p.contentStart || p.contentStart >= p.contentEnd
            || p.contentEnd >= p.closeEnd)
            continue;
        const QVector<Piece> pieces = piecesOf(block);
        const int            id     = (*serial)++;
        const bool           open   = revealed.value(i, false);

        // The content's characters with their piece (words, spaces, links' spaces).
        QVector<QPair<int, int>> words;  // [from, to) of each word bar
        QVector<QPair<int, int>> spaces; // [from, to) of a link's spaces (one piece each)
        if (!open) {
            int wordStart = -1;
            int k         = 0;
            for (int q = p.contentStart; q <= p.contentEnd; ++q) {
                while (k < pieces.size() && pieces.at(k).end <= q)
                    ++k;
                const bool  inside = q < p.contentEnd && k < pieces.size();
                const QChar ch     = inside ? pieces.at(k).text.at(q - pieces.at(k).start) : QChar(QLatin1Char(' '));
                if (!ch.isSpace() && inside) {
                    if (wordStart < 0)
                        wordStart = q;
                    continue;
                }
                if (wordStart >= 0) {
                    // Pieces of at most kMaxPiece characters (never between the halves of a surrogate pair).
                    for (int a = wordStart; a < q;) {
                        int b = qMin(q, a + kMaxPiece);
                        if (b < q && QChar::isHighSurrogate(doc->characterAt(b - 1).unicode()))
                            ++b;
                        words.append({a, b});
                        a = b;
                    }
                    wordStart = -1;
                }
                if (inside && pieces.at(k).format.isAnchor()) {
                    if (!spaces.isEmpty() && spaces.last().second == q && q > pieces.at(k).start)
                        spaces.last().second = q + 1;
                    else
                        spaces.append({q, q + 1});
                }
            }
        }

        QTextCursor c(doc);
        // A link that runs across a marker loses its link outside the pair too, the way back kept (review fix). Before
        // any edit of this pair: the pieces' positions still hold.
        if (!open) {
            for (const QPair<int, int>& r : linkAcrossMarkers(pieces, p.open, p.closeEnd)) {
                for (const Piece& piece : pieces) {
                    const int a = qMax(r.first, piece.start);
                    const int b = qMin(r.second, piece.end);
                    if (a >= b)
                        continue;
                    c.setPosition(a);
                    c.setPosition(b, QTextCursor::KeepAnchor);
                    c.setCharFormat(unlinked(piece.format, id));
                }
            }
        }
        // The closing marker.
        {
            QVariantList runs;
            QString      identity;
            runsOf(pieces, p.contentEnd, p.closeEnd, &runs, &identity);
            c.setPosition(p.contentEnd);
            c.setPosition(p.closeEnd, QTextCursor::KeepAnchor);
            QTextCharFormat f = objectFormat(Close, id, words.size() + 1, runs, identity);
            f.setProperty(kState, open ? 1 : 0);
            c.insertText(QString(QChar::ObjectReplacementCharacter), f);
        }
        // The content: words as bars and a link's spaces without the link (hidden), or tinted text (revealed).
        if (open) {
            for (const Piece& piece : pieces) {
                const int a = qMax(p.contentStart, piece.start);
                const int b = qMin(p.contentEnd, piece.end);
                if (a >= b || piece.format.isImageFormat())
                    continue;
                c.setPosition(a);
                c.setPosition(b, QTextCursor::KeepAnchor);
                c.setCharFormat(tinted(piece.format, id));
            }
        } else {
            int w = words.size() - 1;
            int s = spaces.size() - 1;
            while (w >= 0 || s >= 0) {
                const bool word = s < 0 || (w >= 0 && words.at(w).first > spaces.at(s).first);
                if (word) {
                    const QPair<int, int>& r = words.at(w);
                    QVariantList           runs;
                    QString                identity;
                    runsOf(pieces, r.first, r.second, &runs, &identity);
                    c.setPosition(r.first);
                    c.setPosition(r.second, QTextCursor::KeepAnchor);
                    c.insertText(QString(QChar::ObjectReplacementCharacter), objectFormat(Word, id, w + 1, runs, identity));
                    --w;
                } else {
                    const QPair<int, int>& r = spaces.at(s);
                    for (const Piece& piece : pieces) {
                        const int a = qMax(r.first, piece.start);
                        const int b = qMin(r.second, piece.end);
                        if (a >= b)
                            continue;
                        c.setPosition(a);
                        c.setPosition(b, QTextCursor::KeepAnchor);
                        c.setCharFormat(unlinked(piece.format, id));
                    }
                    --s;
                }
            }
        }
        // The opening marker, with the spoiler's key and state.
        {
            QVariantList runs;
            QString      identity;
            runsOf(pieces, p.open, p.contentStart, &runs, &identity);
            QTextCharFormat f = objectFormat(Open, id, 0, runs, identity);
            f.setProperty(kKey, keys.value(i));
            f.setProperty(kState, open ? 1 : 0);
            c.setPosition(p.open);
            c.setPosition(p.contentStart, QTextCursor::KeepAnchor);
            c.insertText(QString(QChar::ObjectReplacementCharacter), f);
        }
        ++done;
    }
    batch.endEditBlock();
    return done;
}

QVector<Spoiler> spoilersIn(QTextDocument* doc, int blockNumber)
{
    QVector<Spoiler> out;
    if (!doc)
        return out;
    QTextBlock b    = blockNumber >= 0 ? doc->findBlockByNumber(blockNumber) : doc->begin();
    const bool only = blockNumber >= 0;
    for (; b.isValid(); b = b.next()) {
        QHash<int, int> openAt; // id -> index in out
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !isObject(f.charFormat()))
                continue;
            const QTextCharFormat cf = f.charFormat();
            const int             id = cf.intProperty(kId);
            if (kindOf(cf) == Open) {
                Spoiler s;
                s.id       = id;
                s.open     = f.position();
                s.revealed = cf.intProperty(kState) == 1;
                s.key      = cf.stringProperty(kKey);
                s.block    = b.blockNumber();
                openAt.insert(id, out.size());
                out.append(s);
            } else if (kindOf(cf) == Close && openAt.contains(id)) {
                out[openAt.value(id)].close = f.position();
            }
        }
        if (only)
            break;
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const Spoiler& s) { return s.close < 0; }), out.end());
    return out;
}

int wordIdAt(QTextDocument* doc, int position)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1 || doc->characterAt(position) != QChar::ObjectReplacementCharacter)
        return -1;
    const QTextCharFormat f = formatAt(doc, position);
    return isObject(f) && kindOf(f) == Word ? f.intProperty(kId) : -1;
}

bool reveal(QTextDocument* doc, const Spoiler& spoiler)
{
    if (!doc || spoiler.revealed || spoiler.open < 0 || spoiler.close <= spoiler.open)
        return false;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    // The words back as tinted text, last first; then every other piece of its text tinted. The parts of a link that ran
    // across a marker (outside the pair) get their link back as TeamSpeak had it, untinted.
    const QTextBlock    first = doc->findBlock(spoiler.open);
    const QVector<Ours> ours  = oursIn(doc, first.position(), first.position() + first.length());
    int                 end   = spoiler.close;
    for (const Ours& o : ours) {
        if (o.format.intProperty(kId) != spoiler.id)
            continue;
        QTextCursor c(doc);
        c.setPosition(o.position);
        c.setPosition(o.position + o.length, QTextCursor::KeepAnchor);
        if (o.position <= spoiler.open || o.position >= spoiler.close) {
            if (!o.object && kindOf(o.format) == Space)
                giveBack(doc, o); // outside the pair (its markers are handled below)
            continue;
        }
        if (o.object && kindOf(o.format) == Word) {
            const QVariantList runs = o.format.property(kRuns).toList();
            c.removeSelectedText();
            const int before = doc->characterCount();
            insertRuns(c, runs, spoiler.id);
            end += doc->characterCount() - before - 1;
        } else if (!o.object && kindOf(o.format) == Space) {
            c.setCharFormat(tinted(qvariant_cast<QTextFormat>(o.format.property(kBefore)).toCharFormat(), spoiler.id));
        }
    }
    // Spaces and line breaks that stayed text get the tint too (one look from marker to marker).
    const QTextBlock block = doc->findBlock(spoiler.open);
    for (const Piece& piece : piecesOf(block)) {
        const int a = qMax(spoiler.open + 1, piece.start);
        const int b = qMin(end, piece.end);
        if (a >= b || piece.format.isImageFormat() || kindOf(piece.format) != None || isObject(piece.format))
            continue;
        QTextCursor c(doc);
        c.setPosition(a);
        c.setPosition(b, QTextCursor::KeepAnchor);
        c.setCharFormat(tinted(piece.format, spoiler.id));
    }
    // The markers say so.
    for (const int at : {spoiler.open, end}) {
        QTextCursor     c(doc);
        QTextCharFormat f = formatAt(doc, at);
        if (isObject(f) && (kindOf(f) == Open || kindOf(f) == Close) && f.intProperty(kId) == spoiler.id) {
            f.setProperty(kState, 1);
            c.setPosition(at);
            c.setPosition(at + 1, QTextCursor::KeepAnchor);
            c.setCharFormat(f);
        }
    }
    batch.endEditBlock();
    return true;
}

bool unhide(QTextDocument* doc, const Spoiler& spoiler)
{
    if (!doc || spoiler.open < 0 || spoiler.close <= spoiler.open)
        return false;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    // The whole block: a link that ran across a marker was unlinked outside the pair too.
    const QTextBlock block = doc->findBlock(spoiler.open);
    for (const Ours& o : oursIn(doc, block.position(), block.position() + block.length())) {
        const QTextCharFormat mine = o.inner ? emojiformat::originalFormat(o.format) : o.format;
        if (mine.intProperty(kId) == spoiler.id)
            giveBack(doc, o);
    }
    batch.endEditBlock();
    return true;
}

bool holdsOthers(QTextDocument* doc, const Spoiler& spoiler)
{
    if (!doc || spoiler.open < 0 || spoiler.close <= spoiler.open)
        return false;
    const QTextBlock block = doc->findBlock(spoiler.open);
    for (const Piece& piece : piecesOf(block)) {
        if (piece.end <= spoiler.open + 1 || piece.start >= spoiler.close)
            continue;
        if (isObject(piece.format) || kindOf(piece.format) != None)
            continue;
        if (!isTeamSpeakText(piece.format))
            return true;
    }
    return false;
}

int restoreAll(QTextDocument* doc)
{
    if (!doc)
        return 0;
    const QVector<Ours> ours = oursIn(doc, 0, doc->characterCount());
    if (ours.isEmpty())
        return 0;
    int         spoilers = 0;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    for (const Ours& o : ours) {
        spoilers += o.object && kindOf(o.format) == Open ? 1 : 0;
        giveBack(doc, o);
    }
    batch.endEditBlock();
    return spoilers;
}

QColor barColor(const Look& look, int id)
{
    const QColor base = look.base.isValid() ? look.base : QColor(Qt::white);
    const QColor text = look.text.isValid() ? look.text : QColor(Qt::black);
    const qreal  t    = id >= 0 && id == look.pressed ? 0.7 : (id >= 0 && id == look.hovered ? 0.6 : 0.5);
    const auto   mix  = [t](int a, int b) { return qRound(a + (b - a) * t); };
    return QColor(mix(base.red(), text.red()), mix(base.green(), text.green()), mix(base.blue(), text.blue()));
}

QSizeF objectSize(const QTextDocument* doc, const QTextFormat& format)
{
    if (!isObject(format) || kindOf(format) != Word)
        return QSizeF(0, 0);
    const QVariantList runs  = format.property(kRuns).toList();
    qreal              width = 0;
    qreal              height = 0;
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QTextFormat   f = qvariant_cast<QTextFormat>(runs.at(i + 1));
        const QFontMetricsF fm(fontOf(doc, f));
        if (height <= 0)
            height = fm.ascent();
        const QString text = runs.at(i).toString();
        if (f.isImageFormat()) {
            const QTextImageFormat image = f.toImageFormat();
            const qreal            each  = image.width() > 0 ? image.width() : fm.height();
            width += each * qMax(1, static_cast<int>(text.count(QChar::ObjectReplacementCharacter)));
            continue;
        }
        width += fm.horizontalAdvance(text);
    }
    return QSizeF(width, height);
}

// ---- Handler -----------------------------------------------------------------------------------------------

Handler::Handler(QObject* parent)
    : QObject(parent)
{
}

QSizeF Handler::intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format)
{
    Q_UNUSED(posInDocument)
    return objectSize(doc, format);
}

void Handler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument, const QTextFormat& format)
{
    Q_UNUSED(posInDocument)
    if (!painter || kindOf(format) != Word || rect.width() <= 0)
        return;
    const QVariantList runs = format.property(kRuns).toList();
    const QFontMetricsF fm(fontOf(doc, runs.size() >= 2 ? qvariant_cast<QTextFormat>(runs.at(1)) : QTextFormat()));
    // From the line's top to its bottom (the object stands on the baseline with the font's ascent).
    const QRectF bar(rect.x(), rect.y(), rect.width(), fm.ascent() + fm.descent());
    const qreal  radius = qMax(2.0, 0.2 * bar.height());
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(barColor(m_looks.value(doc), format.intProperty(kId)));
    painter->drawRoundedRect(bar, radius, radius);
    painter->restore();
}

void Handler::setLook(const QTextDocument* doc, const Look& look)
{
    m_looks.insert(doc, look);
}

Look Handler::look(const QTextDocument* doc) const
{
    return m_looks.value(doc);
}

void Handler::forget(const QTextDocument* doc)
{
    m_looks.remove(doc);
}

} // namespace textspoilerdoc
