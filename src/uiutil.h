#pragma once

// Small helpers shared by the plugin's widgets and painters. QtCore/QtGui only (no widgets), so the
// render tools and the unit tests can use them too.

#include <QColor>
#include <QString>

namespace ui {

// Windows' "Show animations in Windows" setting (Settings > Accessibility > Visual effects). When it
// is off, spinners become static indicators, fades and slides are skipped, and GIFs only play on
// hover. Not cached: the user can change it while TeamSpeak runs. True if it cannot be read.
bool animationsEnabled();

// Windows' "Dismiss notifications after" setting (Accessibility), in milliseconds (5000 if it cannot
// be read). Messages the user has to read, such as errors, stay at least this long.
int notificationDurationMs();

// WCAG 2 contrast ratio of two colours, from 1.0 (same luminance) to 21.0 (black on white); text
// needs 4.5, large text and icons 3.0. Alpha is ignored: flatten() translucent colours first.
double contrastRatio(const QColor& a, const QColor& b);

// A translucent colour drawn over an opaque background, as the screen shows it.
QColor flatten(const QColor& color, const QColor& background);

// The confirmation after saving a file: "Saved to Pictures" (the folder's name; a drive root is
// shown as "C:\"). Shared by the viewer and the chat so both say the same.
QString savedToText(const QString& savedPath);

} // namespace ui
