#pragma once

// 2.7 Dark mode: the small pictures the dark style sheet shows (design_dark 2.5): check boxes, radio buttons, chevrons,
// the tab's close cross, the menu's check and dot, drawn at their final pixel size for the size factor k in the theme's
// colours and written as PNG files into <baseDir>/darkmode/<round(k * 100)>/. The sheet names them by absolute path.
// A folder whose glyphs.txt says it holds this glyph set in these colours is used as it is (nothing written).

#include <QString>
#include <QStringList>

#include "darkpalette.h"

namespace darkglyphs {

// The folder with every glyph, or an empty string when it can't be written (the sheet is built without pictures then).
QString ensure(const QString& baseDir, qreal k, const darkpalette::Tokens& tokens);
// The glyph names (without ".png") and the pixel size each is drawn at for k (tests).
QStringList names();
int         sizeOf(const QString& name, qreal k);
// What glyphs.txt holds for these tokens and k: the glyph set's version and a hash of the colours.
QString stamp(qreal k, const darkpalette::Tokens& tokens);

} // namespace darkglyphs
