#pragma once

// 2.2 emoji: finds the emoji of the table in text, as whole grapheme clusters: ZWJ sequences, skin
// tones, keycaps, flags (regional-indicator pairs and tag sequences) and variation selectors are part of
// the emoji they belong to. The longest table emoji wins; an unknown ZWJ sequence falls back to the
// emoji it is made of. QtCore only (unit-tested).

#include <QString>
#include <QVector>

namespace emoji {

struct Match {
    int  start    = 0;  // UTF-16 index
    int  length   = 0;  // UTF-16 units, selectors and modifiers included
    int  id       = -1; // emojidata.h
    bool selector = false; // carried U+FE0F (asked to be shown as an emoji)
};

// The emoji in text, in order; at most maxMatches (the rest is left alone).
QVector<Match> findEmoji(const QString& text, int maxMatches = 1000);

// Whether a match is drawn as a picture inside running text. Everything is, except a single code point
// that shows as text by default (©, ™, ↔, digits, ...) and wasn't followed by U+FE0F; a few of those
// that people type meaning the emoji (❤ ☺ ☹ ✌ ☝ ✍ ☠ ❣ ♥ ☀ ☁ ❄ ✈ ⚠) count as emoji anyway.
bool showsAsPicture(const Match& match);

// When text is nothing but emoji shown as pictures and white space: how many (at most maxCount + 1 is
// counted). -1 when anything else is in it, 0 for empty or blank text.
int pictureOnlyCount(const QString& text, int maxCount);

} // namespace emoji
