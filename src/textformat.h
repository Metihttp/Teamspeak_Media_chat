#pragma once

// 2.7 F1: formatting the text in TeamSpeak's chat input before it is sent (the input's right-click menu and
// Alt+Shift+S, inputformat.h). Bold, Italic and Underline are TeamSpeak's own BBCode as its Format menu writes it
// ("[B]..[/B]", "[I]..[/I]", "[U]..[/U]": everyone sees them); Spoiler is "[spoiler]..[/spoiler]" (textspoiler.h).
// Pure (QtCore): the input's plain text and its selection in, one edit out.
//
// apply():
//  1. The selection shrinks past white space at either end (a drag that took the space).
//  2. When isApplied, the tags go and the text inside stays selected (toggle off).
//  3. Otherwise the selection first grows over every pair of any of the four formats that it cuts (one tag inside,
//     the other outside), then pairs of the same format inside it go (no nesting; for Spoiler, required), then
//     a bare web address that ends exactly at the selection's end gets "[URL]..[/URL]" (the client's own wrapping
//     of bare addresses can never take the closing tag into the address), then open tag + text + close tag, the
//     text inside selected.
//  4. Nothing selected (or only white space): an empty pair at the cursor, the cursor between the tags.
// Multi-line selections are wrapped as a whole.

#include <QString>

namespace textformat {

enum class Format { Bold, Italic, Underline, Spoiler };

QString openTag(Format f);  // "[B]" "[I]" "[U]" "[spoiler]"
QString closeTag(Format f); // "[/B]" "[/I]" "[/U]" "[/spoiler]"

struct Edit {
    int     from = 0, to = 0; // replace text[from, to) ...
    QString replacement;      // ... by this
    int     selStart = 0, selEnd = 0; // then select this (equal: just the cursor)
    bool    ok = false;
    bool    valid() const { return ok; }
};

// The menu's tick: the trimmed selection is one balanced pair of f (its tags at both ends, inside it or right
// around it), or (Spoiler) it lies inside one spoiler's text.
bool isApplied(const QString& text, int selStart, int selEnd, Format f);
Edit apply(const QString& text, int selStart, int selEnd, Format f);
// text with the edit made (tests).
QString applied(const QString& text, const Edit& edit);

} // namespace textformat
