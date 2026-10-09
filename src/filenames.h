#pragma once

// Local file names for files that come from chat (2.2 drag-out). Pure QtCore: unit-tested.
// Names in chat links are untrusted (anyone can post one), so whatever reaches the local disk goes
// through safeLocalFileName() first: the media cache, "Save as" suggestions and dragged-out copies.

#include <QString>

struct MediaLink;

namespace filenames {

// Windows ignores trailing dots and spaces ("x.exe." opens x.exe).
QString chopTrailingDotsAndSpaces(QString name);

// CON, PRN, AUX, NUL, COM0-9, LPT0-9 (also with superscript ¹²³), CONIN$ and CONOUT$, in any
// case and with any extension ("nul.txt", "Com1.tar.gz"): names Windows maps to devices.
bool isReservedDeviceName(const QString& name);

// A name safe for the local file system: reserved characters (<>:"/\|?*, ':' would even create an
// NTFS stream) and control characters become '_', trailing dots and spaces go, device names get a
// leading '_' ("CON.txt" -> "_CON.txt"), and the length is at most maxLength with the extension
// kept. Empty -> "file".
QString safeLocalFileName(const QString& name, int maxLength = 120);

// The clean name a file dragged or copied out of the chat gets: what the chat shows
// (displayNameFor(): "holiday_3f9a1c2e.jpg" -> "holiday.jpg", without bidi marks) made safe, and
// short enough that <folder>/<name> stays within maxPathLength characters (folderLength: the
// folder's length including its trailing separator).
QString exportFileName(const MediaLink& link, int folderLength = 0, int maxPathLength = 240);

} // namespace filenames
