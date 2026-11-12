#pragma once

// 2.5 link previews: the one text cleaner for every text that comes from a website or an announcement
// (titles, authors, site names, descriptions). Pure QtCore (unit-tested in tst_textclean); content::cleanText,
// announcement's cleanText and linkcard::cleanText are thin wrappers around it.
//
// The result is one line of plain text that can't hide or reorder anything around it:
//  1. every white-space character (also TAB, CR, LF, NEL, U+2028/2029) and every run of them -> one space;
//     the text is trimmed;
//  2. removed: controls (Cc); format characters (Cf) except ZWNJ U+200C and ZWJ U+200D; U+061C, U+200B,
//     U+200E/F, U+202A-202E, U+2060-2064, U+2066-206F, U+FEFF, U+FFF9-FFFC, U+FFFD; the invisible fillers
//     U+115F, U+1160, U+3164, U+FFA0, U+2800, U+034F, U+17B4, U+17B5, U+180E; tag characters U+E0000-E007F;
//     variation selectors U+E0100-E01EF; noncharacters; private use (U+E000-F8FF, planes 15-16); lone
//     surrogates;
//  3. ZWNJ and ZWJ are kept only between two visible characters (a letter, mark, number, symbol or
//     punctuation): Persian words and emoji sequences stay whole, a joiner anywhere else goes;
//  4. U+FE00-FE0F only right after a visible character;
//  5. at most 2 combining marks (Mn, Me) after one base character (no "Zalgo" text painting over its neighbours).
// Cutting (both functions): the text is cleaned first, then cut at the last grapheme boundary that fits (never
// half a surrogate pair, never a mark without its base, never inside an emoji ZWJ sequence), and the cut text is
// cleaned again (a joiner left at the new end goes; trailing white space is trimmed).

#include <QString>

namespace linkpreview::textclean {

// At most maxChars UTF-16 units, with "…" at the end when the text was cut.
QString clean(const QString& text, int maxChars);
// At most maxBytes bytes of UTF-8 (whole graphemes, no "…"): the wire form of a text.
QString cleanToBytes(const QString& text, int maxBytes);

// The UTF-8 size of text (what cleanToBytes counts).
int utf8Bytes(const QString& text);

} // namespace linkpreview::textclean
