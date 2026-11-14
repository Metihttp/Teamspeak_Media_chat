#pragma once

// 2.6: the Settings dialog's label roles, shared with the Chat proxy window (design 1.3.8). A QLabel with the property
// "role" = "hint" (secondary text), "error" (a problem; always with the warning icon) or "ok" (a check that passed;
// always with the painted check mark) gets its colour here: hint 4.5:1 toward the background, error red and ok green,
// each replaced by the window text colour when it would read below 4.5:1 on the window. Meaning never rests on colour
// (every state has words). Style sheets are built with fromLatin1 (TeamSpeak's QStyleSheetStyle keeps the parsed text
// after an unload), scoped to the given object name.

#include <QColor>
#include <QPalette>
#include <QPixmap>
#include <QString>

class QWidget;

namespace settingstheme {

struct Colors {
    QColor hint;
    QColor error;
    QColor ok;
    bool   dark = false;
};

bool   isDark(const QPalette& palette);
// The text colour moved toward the background as far as start allows while it keeps minContrast.
QColor mutedText(const QPalette& palette, double start, double minContrast);
Colors roleColors(const QPalette& palette);
// Every labelled QLabel under root gets its role's colour (a palette: enough without a style sheet).
void applyLabelRoles(QWidget* root, const Colors& colors);
// The style sheet rules for TeamSpeak's skins (they colour labels by style sheet, which beats a palette).
QString roleSheet(const QString& objectName, const Colors& colors);
// The check mark of an "ok" row: two strokes, 1.6 px at the device pixel ratio, in color.
QPixmap checkMark(const QColor& color, int size, qreal dpr);

} // namespace settingstheme
