#pragma once

// 2.7 F1: text spoilers as text. A spoiler is "[spoiler]the text[/spoiler]" in a message: TeamSpeak shows unknown tags
// literally, so everyone without TS Media 2.7 (vanilla TeamSpeak, TS Media 2.6 and older, ServerQuery, chat logs) reads
// the tags and the text, and TS Media 2.7 hides the text until it is clicked (textspoilerdoc.h, chattextspoilers.h).
// TS Media writes the markers in lower case and reads them in any case. Pure (QtCore only).
//
// Pairing: left to right, an opening marker pairs with the first closing marker after it; no nesting (an opening
// marker inside a pair is part of its text). Not a spoiler (shown as typed): an opening marker without a closing one,
// and a pair whose content is empty or white space only. At most kMaxPairs pairs per message.

#include <QString>
#include <QVector>

namespace textspoiler {

constexpr int kMaxPairs = 32;

QString openMarker();  // "[spoiler]"
QString closeMarker(); // "[/spoiler]"
// What a reply's quote, a snippet and a copy show for a spoiler. Fixed (it is part of the quote line that other
// clients match): "(spoiler)".
QString placeholder();

struct Range {
    int open         = 0; // the opening marker's first character
    int contentStart = 0; // right after it
    int contentEnd   = 0; // the closing marker's first character
    int closeEnd     = 0; // right after the closing marker
};

// The pairs of text, in order.
QVector<Range> ranges(const QString& text, int max = kMaxPairs);
// True if text holds an opening marker anywhere (any case): a cheap test before parsing.
bool mayHold(const QString& text);
// text with every pair replaced by placeholder(). openToEnd: an opening marker without a closing one hides the rest
// of the text too (a snippet that a cut or an older TS Media's quote ended inside a spoiler).
QString masked(const QString& text, bool openToEnd = false);
// A message's BBCode without its spoilers (each pair removed with its markers; a "[URL]" tag inside goes with it):
// what the sender's link previews and the GIFs tab read, so a link inside a spoiler is never previewed or remembered.
QString withoutHidden(const QString& bbcode);

} // namespace textspoiler
