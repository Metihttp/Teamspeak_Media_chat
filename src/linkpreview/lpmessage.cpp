#include "lpmessage.h"

#include <QRegularExpression>
#include <QVector>

#include "replies.h"
#include "textreact.h"

namespace linkpreview::message {

namespace {

constexpr int kMaxTagChars  = 256;   // a longer "[...]" is text
constexpr int kMaxReadChars = 16384; // what is read of a message (TeamSpeak's limit is 8192 bytes)

struct Piece {
    QString text;
    QString href; // a link's target; empty: plain text
};

// BBCode tags TeamSpeak 3 formats with (their text is left out of what it shows).
bool isFormatTag(const QString& name)
{
    static const char* const tags[] = {"b", "i", "u", "s", "color", "size", "left", "right", "center", "list", "*", "hr", "table", "tr", "td", "th", "img", "noparse"};
    for (const char* tag : tags) {
        if (name == QLatin1String(tag))
            return true;
    }
    return false;
}

// The message without a reply's quote line (its first line, when that reads as one).
QString bodyOf(const QString& bbcode)
{
    const QString text = bbcode.left(kMaxReadChars);
    const int     nl   = text.indexOf(QLatin1Char('\n'));
    const QString first = nl < 0 ? text : text.left(nl);
    if (first.size() <= replies::kMaxQuoteLine && replies::parseQuoteBBCode(first).valid())
        return nl < 0 ? QString() : text.mid(nl + 1);
    return text;
}

// The message as TeamSpeak shows it: plain text and links (label, target).
QVector<Piece> parse(const QString& body)
{
    QVector<Piece> out;
    QString        text;
    QString        href;
    bool           inLink    = false;
    bool           plainLink = false; // [URL]x[/URL]: the target is the text
    auto flush = [&] {
        if (text.isEmpty())
            return;
        out.append({text, inLink ? href : QString()});
        text.clear();
    };
    for (int i = 0; i < body.size(); ++i) {
        const QChar c = body.at(i);
        if (c == QLatin1Char('\\')) {
            // A run of backslashes right before a bracket shows as nothing; the bracket is text.
            int j = i;
            while (j < body.size() && body.at(j) == QLatin1Char('\\'))
                ++j;
            if (j < body.size() && (body.at(j) == QLatin1Char('[') || body.at(j) == QLatin1Char(']'))) {
                text += body.at(j);
                i = j;
            } else {
                text += body.mid(i, j - i);
                i = j - 1;
            }
            continue;
        }
        if (c != QLatin1Char('[')) {
            text += c;
            continue;
        }
        const int close = body.indexOf(QLatin1Char(']'), i + 1);
        if (close < 0 || close - i > kMaxTagChars) {
            text += c;
            continue;
        }
        const QString inner   = body.mid(i + 1, close - i - 1);
        const bool    closing = inner.startsWith(QLatin1Char('/'));
        const int     eq      = inner.indexOf(QLatin1Char('='));
        const QString name    = (closing ? inner.mid(1) : eq < 0 ? inner : inner.left(eq)).trimmed().toLower();
        if (name == QLatin1String("url")) {
            if (closing) {
                if (inLink) {
                    if (plainLink)
                        href = text.trimmed();
                    flush();
                    inLink = false;
                }
            } else if (!inLink) {
                flush();
                inLink    = true;
                plainLink = eq < 0;
                href      = plainLink ? QString() : inner.mid(eq + 1).trimmed();
            }
            i = close;
            continue;
        }
        if (isFormatTag(name) && (!closing || eq < 0)) {
            i = close;
            continue;
        }
        text += c; // not a tag of TeamSpeak's: shown as written
    }
    if (inLink && plainLink)
        href.clear(); // a [URL] never closed is no link
    flush();
    return out;
}

} // namespace

QString visibleText(const QString& bbcode)
{
    QString out;
    for (const Piece& piece : parse(bodyOf(bbcode)))
        out += piece.text;
    return out;
}

QString identityHash(const QString& senderUid, const QString& bbcode)
{
    return textreact::contentHash(senderUid, visibleText(bbcode));
}

QList<QUrl> webLinks(const QString& bbcode, int max)
{
    static const QRegularExpression bare(QStringLiteral("https?://[^\\s<>\"\\[\\]]+"), QRegularExpression::CaseInsensitiveOption);
    QList<QUrl> out;
    auto add = [&](const QString& href, bool tagged) {
        QString target = href.trimmed();
        // Punctuation right after an address belongs to the sentence.
        while (!target.isEmpty() && QStringLiteral(".,;:!?)'").contains(target.at(target.size() - 1)))
            target.chop(1);
        // A [URL] of an address typed without a scheme: the chat links it as http:// (classify() takes that
        // for YouTube only).
        if (tagged && target.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
            target.prepend(QLatin1String("http://"));
        if (!target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) && !target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
            return;
        const QUrl url(target, QUrl::TolerantMode);
        if (url.isValid() && out.size() < max)
            out.append(url);
    };
    for (const Piece& piece : parse(bodyOf(bbcode))) {
        if (out.size() >= max)
            break;
        if (!piece.href.isEmpty()) {
            add(piece.href, true);
            continue;
        }
        for (auto it = bare.globalMatch(piece.text); it.hasNext() && out.size() < max;)
            add(it.next().captured(0), false);
    }
    return out;
}

QStringList linkHrefs(const QString& bbcode, int max)
{
    static const QRegularExpression bare(QStringLiteral("https?://[^\\s<>\"\\[\\]]+"), QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    auto add = [&](const QString& href, bool tagged) {
        if (out.size() >= max)
            return;
        QString target = href.trimmed();
        // A [URL] of an address typed without a scheme: the chat links it as http:// (classify() fetches https).
        if (tagged && target.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
            target.prepend(QLatin1String("http://"));
        if (!target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) && !target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
            return;
        out.append(target); // as written: the end is trimmed by trimLinkEnd() on both sides alike
    };
    // Live test fix: TeamSpeak shows an address without [URL] as a link only at the start of the message or after
    // white space; "(https://...)", "x:https://...", "\"https://..." and an address right after a tag's text stay
    // plain text on every receiver, so the sender makes no preview of them either.
    QChar before = QLatin1Char(' ');
    for (const Piece& piece : parse(bodyOf(bbcode))) {
        if (out.size() >= max)
            break;
        if (!piece.href.isEmpty()) {
            add(piece.href, true);
        } else {
            for (auto it = bare.globalMatch(piece.text); it.hasNext() && out.size() < max;) {
                const QRegularExpressionMatch m    = it.next();
                const QChar                   prev = m.capturedStart(0) > 0 ? piece.text.at(m.capturedStart(0) - 1) : before;
                if (prev.isSpace())
                    add(m.captured(0), false);
            }
        }
        if (!piece.text.isEmpty())
            before = piece.text.at(piece.text.size() - 1);
    }
    return out;
}

bool isMediaMessage(const QString& bbcode)
{
    return bbcode.contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

} // namespace linkpreview::message
