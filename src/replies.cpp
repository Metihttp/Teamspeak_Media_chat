#include "replies.h"

#include <QRegularExpression>
#include <QUrl>

#include "albums.h"    // uidFromClientHref: the same reading of TeamSpeak's client links
#include "medialink.h" // escapedMessageSize, bbcodeLiteral, sanitizeCaption

namespace replies {

namespace {

const QChar kArrow(0x21AA);
const QChar kRatio(0x2236);
const QChar kEllipsis(0x2026);
const QChar kOpenQuote(0x201C);
const QChar kCloseQuote(0x201D);

// Invisible characters a snippet or name never needs, bidi marks included (they could reorder what is
// drawn). ZWNJ and ZWJ stay: Persian words and emoji use them; line breaks and tabs stay too: they
// become spaces later.
bool isInvisible(QChar ch)
{
    const ushort u = ch.unicode();
    if (u == 0x200E || u == 0x200F || u == 0x061C || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069))
        return true;
    return u == 0x200B || u == 0x2060 || u == 0xFEFF || u == 0xFFFC || u == 0x00AD || (ch.category() == QChar::Other_Control && !ch.isSpace());
}

QString withoutInvisible(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (!isInvisible(ch))
            out += ch;
    }
    return out;
}

bool validUid(const QString& uid)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9+/=_.\\-]{1,64}$"));
    return re.match(uid).hasMatch();
}

// A nickname from a header or a quote line: one line, no objects, at most kMaxNickChars.
QString cleanNick(const QString& raw)
{
    QString nick = withoutInvisible(raw).trimmed();
    for (const QChar ch : nick) {
        if (ch == QChar::LineSeparator || ch == QChar::ParagraphSeparator || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            return {};
    }
    if (nick.size() > kMaxNickChars)
        return {};
    return nick;
}

// TeamSpeak shows a run of backslashes right before a bracket as nothing (S0); everything else as is.
QString unescapeBBCode(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\')) {
            int j = i;
            while (j < text.size() && text.at(j) == QLatin1Char('\\'))
                ++j;
            if (j < text.size() && (text.at(j) == QLatin1Char('[') || text.at(j) == QLatin1Char(']'))) {
                i = j - 1; // the run goes, the bracket follows
                continue;
            }
            out += text.mid(i, j - i);
            i = j - 1;
            continue;
        }
        out += text.at(i);
    }
    return out;
}

int clientIdOf(const QString& href)
{
    static const QRegularExpression re(QStringLiteral("^client://(\\d{1,5})/"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch   m = re.match(href);
    if (!m.hasMatch())
        return 0;
    const int id = m.captured(1).toInt();
    return id > 0 && id <= 0xFFFF ? id : 0;
}

// The nickname at the end of a client link ("...~Nick"), percent-decoded.
QString nickOfHref(const QString& href)
{
    const int tilde = href.indexOf(QLatin1Char('~'));
    if (tilde < 0)
        return {};
    return cleanNick(QUrl::fromPercentEncoding(href.mid(tilde + 1).left(256).toUtf8()));
}

} // namespace

QString arrow()
{
    return QString(kArrow);
}

QString visibleText(const QString& text)
{
    return withoutInvisible(text);
}

QString timeColon()
{
    return QString(kRatio);
}

QString clip()
{
    return QString(QChar(0xD83D)) + QChar(0xDCCE); // U+1F4CE PAPERCLIP
}

// ---- building ------------------------------------------------------------------------------------------

QString snippetSource(const QString& text, const QString& mediaLabel)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    const QString label = mediaLabel.trimmed();
    return label.isEmpty() ? QString() : clip() + QLatin1Char(' ') + label;
}

QString makeSnippet(const QString& source, int maxChars)
{
    if (maxChars <= 0)
        return {};
    static const QRegularExpression url(QStringLiteral("(?:\\b[A-Za-z][A-Za-z0-9+.\\-]{1,15}://|\\bwww\\.)\\S+"));
    QString text = withoutInvisible(source.left(kMaxBodyChars));
    text.replace(url, QStringLiteral("(link)"));
    text = sanitizeCaption(text); // one line, no controls or bidi marks, whitespace collapsed
    if (text.size() <= maxChars)
        return text;
    int cut = maxChars;
    if (text.at(cut - 1).isHighSurrogate())
        --cut;
    const int space = text.lastIndexOf(QLatin1Char(' '), cut);
    if (space >= maxChars * 2 / 3)
        cut = space;
    QString out = text.left(cut);
    while (!out.isEmpty() && (out.endsWith(QLatin1Char(' ')) || out.endsWith(QLatin1Char(',')) || out.endsWith(QLatin1Char('.'))))
        out.chop(1);
    return out + kEllipsis;
}

QString formatTime(int minutes)
{
    if (minutes < 0)
        return {};
    minutes %= 24 * 60;
    return QStringLiteral("%1%2%3").arg(minutes / 60, 2, 10, QLatin1Char('0')).arg(kRatio).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString quoteLine(const Original& original, int snippetChars)
{
    QString nick = cleanNick(sanitizeCaption(original.nick));
    if (nick.isEmpty())
        nick = QStringLiteral("?");
    // A backslash right before "[/URL]" would make TeamSpeak show the closing tag as text.
    QString label = bbcodeLiteral(nick);
    while (label.endsWith(QLatin1Char('\\')))
        label.chop(1);
    QString who = label;
    if (validUid(original.uid)) {
        const int     id      = original.clientId > 0 && original.clientId <= 0xFFFF ? original.clientId : 0;
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(nick, QByteArray(), QByteArrayLiteral("~")));
        who                   = QStringLiteral("[URL=client://%1/%2~%3]%4[/URL]").arg(id).arg(original.uid, encoded, label);
    }
    QString line = QStringLiteral("[i]") + kArrow + QLatin1Char(' ') + who;
    if (original.minutes >= 0)
        line += QStringLiteral(" · ") + formatTime(original.minutes);
    const QString snippet = makeSnippet(snippetSource(original.text, original.mediaLabel), snippetChars);
    if (!snippet.isEmpty())
        line += QStringLiteral(": ") + kOpenQuote + bbcodeLiteral(snippet) + kCloseQuote;
    return line + QStringLiteral("[/i]");
}

QString composeReply(const Original& original, const QString& text, int maxBytes)
{
    // As TeamSpeak would send it: "\n" line breaks only (S0: "\r\n" shows a stray space), no trailing
    // whitespace and no empty lines in front.
    QString body = text;
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    body.replace(QChar::LineSeparator, QLatin1Char('\n'));
    body.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    while (!body.isEmpty() && body.at(body.size() - 1).isSpace())
        body.chop(1);
    while (!body.isEmpty() && (body.at(0) == QLatin1Char('\n')))
        body.remove(0, 1);
    if (body.trimmed().isEmpty())
        return {};
    for (const int chars : {kSnippetChars, 40, 24, 0}) {
        const QString message = quoteLine(original, chars) + QLatin1Char('\n') + body;
        if (escapedMessageSize(message) <= maxBytes)
            return message;
    }
    return {};
}

// ---- reading ---------------------------------------------------------------------------------------------

Quote parseQuote(const QVector<Run>& runs)
{
    struct Link {
        int     start = 0;
        int     end   = 0;
        QString href;
    };
    QString       text;
    QVector<Link> links;
    for (const Run& run : runs) {
        if (text.size() + run.text.size() > kMaxQuoteLine)
            return {};
        const int start = text.size();
        text += run.text;
        if (run.href.isEmpty())
            continue;
        if (!run.href.startsWith(QLatin1String("client://"), Qt::CaseInsensitive))
            return {}; // a quote line of ours links nothing else (web addresses became "(link)")
        if (!links.isEmpty() && links.last().href == run.href && links.last().end == start)
            links.last().end = text.size(); // one link over several pieces
        else
            links.append({start, text.size(), run.href.left(1024)});
    }

    int pos = 0;
    while (pos < text.size() && (text.at(pos).isSpace() || text.at(pos) == QChar::Nbsp))
        ++pos;
    if (pos >= text.size() || text.at(pos) != kArrow)
        return {};
    ++pos;
    while (pos < text.size() && (text.at(pos) == QLatin1Char(' ') || text.at(pos) == QChar::Nbsp))
        ++pos;

    Quote quote;
    int   nameEnd = -1;
    bool  linked  = false;
    for (const Link& link : qAsConst(links)) {
        if (link.start > pos || link.end <= pos)
            continue;
        if (!link.href.startsWith(QLatin1String("client://"), Qt::CaseInsensitive))
            break;
        quote.nick = cleanNick(text.mid(link.start, link.end - link.start));
        if (quote.nick.isEmpty())
            quote.nick = nickOfHref(link.href);
        quote.uid      = albums::uidFromClientHref(link.href);
        quote.clientId = quote.uid.isEmpty() ? 0 : clientIdOf(link.href);
        nameEnd        = link.end;
        linked         = true;
        break;
    }
    if (nameEnd < 0) {
        // No client link (or TeamSpeak didn't make one): the name runs up to the time or the snippet.
        int end = text.size();
        for (const QString& stop : {QStringLiteral(" · "), QStringLiteral(": ") + kOpenQuote}) {
            const int at = text.indexOf(stop, pos);
            if (at >= 0)
                end = qMin(end, at);
        }
        quote.nick = cleanNick(text.mid(pos, end - pos));
        nameEnd    = end;
    }
    if (quote.nick.isEmpty())
        return {};

    static const QRegularExpression rest(QStringLiteral("^[ \\x{00A0}]*(?:\\x{00B7}[ \\x{00A0}]*(\\d{1,2})[:\\x{2236}](\\d{2})[ \\x{00A0}]*)?(?::[ \\x{00A0}]*\\x{201C}(.*)\\x{201D})?[ \\x{00A0}]*$"));
    const QRegularExpressionMatch   m = rest.match(text.mid(nameEnd));
    if (!m.hasMatch())
        return {};
    // Without the client link, a name alone could be any line starting with the arrow: the time or the
    // snippet has to be there too.
    if (!linked && m.capturedStart(1) < 0 && m.capturedStart(3) < 0)
        return {};
    if (!m.captured(1).isEmpty()) {
        const int hours   = m.captured(1).toInt();
        const int minutes = m.captured(2).toInt();
        if (hours > 23 || minutes > 59)
            return {};
        quote.minutes = hours * 60 + minutes;
    }
    QString snippet = withoutInvisible(m.captured(3)).trimmed().left(kMaxSnippet);
    if (snippet.startsWith(clip())) {
        quote.media = true;
        snippet     = snippet.mid(clip().size()).trimmed();
    }
    quote.snippet = snippet;
    return quote;
}

Quote parseQuoteBBCode(const QString& raw)
{
    QString line = raw.trimmed();
    if (line.startsWith(QLatin1String("[i]"), Qt::CaseInsensitive))
        line = line.mid(3);
    if (line.endsWith(QLatin1String("[/i]"), Qt::CaseInsensitive))
        line.chop(4);
    static const QRegularExpression link(QStringLiteral("\\[URL=([^\\]]*)\\](.*?)\\[/URL\\]"), QRegularExpression::CaseInsensitiveOption);
    QVector<Run> runs;
    int          at = 0;
    for (auto it = link.globalMatch(line); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() > at)
            runs.append({unescapeBBCode(line.mid(at, m.capturedStart() - at)), QString()});
        runs.append({unescapeBBCode(m.captured(2)), m.captured(1)});
        at = m.capturedEnd();
    }
    if (at < line.size())
        runs.append({unescapeBBCode(line.mid(at)), QString()});
    return parseQuote(runs);
}

// ---- matching -------------------------------------------------------------------------------------------

QString normalized(const QString& text)
{
    const QVector<uint> codes = text.left(kMaxBodyChars).toUcs4();
    QVector<uint>       kept;
    kept.reserve(codes.size());
    for (const uint code : codes) {
        if (QChar::isLetterOrNumber(code))
            kept.append(QChar::toCaseFolded(code));
    }
    return QString::fromUcs4(kept.constData(), kept.size());
}

QString matchKey(const QString& source)
{
    return normalized(makeSnippet(source, kMaxBodyChars));
}

bool snippetMatches(const Quote& quote, const QString& key)
{
    QString   snippet = quote.snippet;
    const bool cut    = snippet.endsWith(kEllipsis);
    if (cut)
        snippet.chop(1);
    const QString wanted = normalized(snippet);
    if (wanted.isEmpty())
        return true; // left out, or only symbols: the author and the time decide
    return cut ? key.startsWith(wanted) : key == wanted;
}

int timeScore(int quoted, int candidate)
{
    if (quoted < 0 || candidate < 0)
        return 1;
    const int day      = 24 * 60;
    const int distance = ((quoted - candidate) % day + day) % day;
    if (distance <= 1 || distance >= day - 1)
        return 3;
    const int phase = distance % 15;
    return phase <= 1 || phase >= 14 ? 2 : 0;
}

int findOriginal(const Quote& quote, QVector<Candidate>& all, int before)
{
    if (!quote.valid())
        return -1;
    int best      = -1;
    int bestScore = -1;
    int looked    = 0;
    for (int i = qMin(before, all.size()) - 1; i >= 0 && looked < kMaxSearchBack; --i, ++looked) {
        Candidate& c    = all[i];
        const bool same = !quote.uid.isEmpty() && !c.uid.isEmpty() ? c.uid == quote.uid : (!c.nick.isEmpty() && c.nick == quote.nick);
        if (!same)
            continue;
        if (!c.keyed) {
            c.key   = matchKey(c.source);
            c.keyed = true;
        }
        if (quote.media != c.source.startsWith(clip()) && !quote.snippet.isEmpty())
            continue; // a file's name never matches a text, nor the other way round
        if (!snippetMatches(quote, c.key))
            continue;
        const int score = timeScore(quote.minutes, c.minutes);
        if (score > bestScore) {
            best      = i;
            bestScore = score;
            if (score == 3)
                break; // the newest one at the same minute
        }
    }
    return best;
}

} // namespace replies
