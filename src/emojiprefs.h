#pragma once

// 2.2 emoji: what the picker remembers: recently used emoji (most recent first) and the skin tone. Kept
// in <plugin data>/emoji.ini as wire codes (emojidata.h), so a table of another version reads them.
// QtCore only (unit-tested). GUI thread.

#include <QString>
#include <QVector>

namespace emoji::prefs {

constexpr int kMaxRecent = 36; // four rows of the picker

// Where they are kept; empty: in memory only (tests, tools). Loads the file.
void setFile(const QString& path);

QVector<int> recent();          // valid table ids, most recent first; a few common ones until something is used
void         noteUsed(int id);  // moves id to the front and saves
void         clearRecent();

int  tone();       // 0 (none) .. kToneCount
void setTone(int tone);

// The quick reactions (8): the six every TS Media 2.2 knows, then your two most recent others (or 🎉 💯).
QVector<int> quickReactions();

} // namespace emoji::prefs
