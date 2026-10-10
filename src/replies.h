#pragma once

// 2.2 reply: the reply format and how a reply finds the message it answers. Pure logic (QtCore only):
// the chat side is replydoc.* (TeamSpeak's chat document) and chatreplies.* (menus, the reply bar,
// sending); the tests drive this directly.
//
// A reply is a normal TeamSpeak message whose first line quotes the original, so people without the
// plugin read it as a quote and plugin users see a Discord-style reply line instead:
//
//   [i]↪ [URL=client://<clid>/<uid>~<nick>]<nick>[/URL] · 21∶14: “first ~60 characters…”[/i]
//   <the reply text>
//
// * The author's name is TeamSpeak's own client link (the form TeamSpeak puts into the chat input when
//   a client is dragged there); clid is the author's current client id, or 0 when they aren't on the
//   server. The nickname in the link is percent-encoded, the label is bbcodeLiteral()'d.
// * The time is the original's "HH∶mm" with U+2236 RATIO instead of a colon: TeamSpeak turns ":0" (as in
//   "21:05") and "8)" into emoticon pictures, the ratio sign keeps the time readable. Left out when the
//   original's header has no time (timestamps off).
// * The snippet is the original's text on one line (URLs become "(link)"), or "📎 <link label>" for a
//   message that is only a file; left out (with its ": “…”") when the message would not fit otherwise.
// Everything read back from a chat is untrusted: the parser only ever yields plain strings for drawing
// and matching, and bounds what it looks at.

#include <QString>
#include <QStringList>
#include <QVector>

namespace replies {

constexpr int kSnippetChars  = 60;   // the quoted part of the original (UTF-16 units, plus "…")
constexpr int kMaxQuoteLine  = 600;  // a longer first line is not a quote line
constexpr int kMaxNickChars  = 64;
constexpr int kMaxSnippet    = 240;  // what the parser keeps of a quoted snippet
constexpr int kMaxBodyChars  = 4000; // what is read of a message's text for snippets and matching
constexpr int kMaxSearchBack = 1500; // messages looked through for an original, newest first

// U+21AA, the arrow that starts a quote line; U+2236 in its time; U+1F4CE before a file's name.
QString arrow();
QString timeColon();
QString clip();
// text without invisible characters and bidi marks (names and snippets from the chat are drawn as is).
QString visibleText(const QString& text);
// The first max UTF-16 units of text, one less when that would split a surrogate pair (an emoji).
QString leftChars(const QString& text, int max);

// The message being replied to, as read from the chat.
struct Original {
    QString nick;
    QString uid;           // TeamSpeak's unique id from the header's client link; empty if unknown
    int     clientId = 0;  // current client id of the author (0: not on the server)
    int     minutes  = -1; // the header's time as hours * 60 + minutes, -1 without a time
    QString text;          // its text (replydoc's Message::text): caption or plain text
    QString mediaLabel;    // the label of its first file link, if any
};

// What a quote line says.
struct Quote {
    QString nick;
    QString uid;           // empty: no client link (or an invalid one)
    int     clientId = 0;
    int     minutes  = -1;
    QString snippet;       // as quoted (may end in "…"); empty when left out. For a file: its name
    bool    media = false; // the snippet named a file ("📎 …")
    bool    valid() const { return !nick.isEmpty(); }
};

// ---- building --------------------------------------------------------------------------------------

// What a reply quotes of a message: its text, else "📎 " + the file's label, else nothing.
QString snippetSource(const QString& text, const QString& mediaLabel);
// source on one line, web addresses replaced by "(link)", cut at about maxChars (at a space when one is
// near) with "…". Controls, bidi marks and invisible characters are removed.
QString makeSnippet(const QString& source, int maxChars = kSnippetChars);
// "21∶05" for 21 * 60 + 5; empty for a negative value.
QString formatTime(int minutes);
// The quote line as BBCode (no line break). snippetChars 0 leaves the snippet out.
QString quoteLine(const Original& original, int snippetChars = kSnippetChars);
// quoteLine + "\n" + text, with the snippet shortened (then left out) so that the message's escaped size
// stays within maxBytes. Empty when even the shortest quote line doesn't fit, or text is empty.
QString composeReply(const Original& original, const QString& text, int maxBytes);

// ---- reading ----------------------------------------------------------------------------------------

// One piece of a quote line as TeamSpeak shows it: its text and, for a link, where it goes.
struct Run {
    QString text;
    QString href;
};
// The quote line (the message's first line, without its line break) read back. Not a quote: invalid.
Quote parseQuote(const QVector<Run>& runs);
// The same from BBCode as it was sent (tests; also takes the plain form without the client link).
Quote parseQuoteBBCode(const QString& line);

// ---- matching ---------------------------------------------------------------------------------------

// Letters and digits only, case-folded: emoticon pictures, punctuation and spacing don't matter.
QString normalized(const QString& text);
// What snippets are compared with: normalized(makeSnippet(source, no limit)).
QString matchKey(const QString& source);
// Does a quoted snippet name a message whose matchKey is key? A cut snippet matches by its start.
bool snippetMatches(const Quote& quote, const QString& key);
// How well a quoted time fits a message's time: 3 the same minute (±1), 2 a whole number of quarter
// hours apart (another time zone, ±1 minute), 1 unknown, 0 neither.
int timeScore(int quoted, int candidate);

// A message that could be the original.
struct Candidate {
    QString uid;
    QString nick;
    int     minutes = -1;
    QString source;        // snippetSource of the message
    QString key;           // matchKey(source), filled in by findOriginal when first needed
    bool    keyed = false;
};
// The original of a reply among all[0 .. before-1] (document order): the newest message by the quoted
// author (uid, or the nickname when either side has no uid) whose text the snippet names, preferring
// the best timeScore; at most kMaxSearchBack messages back. -1 if there is none.
int findOriginal(const Quote& quote, QVector<Candidate>& all, int before);

} // namespace replies
