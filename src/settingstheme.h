#pragma once

// 2.6: the Settings dialog's label roles, shared with the Chat proxy window (design 1.3.8). A QLabel with the property
// "role" = "hint" (secondary text), "error" (a problem; always with the warning icon) or "ok" (a check that passed;
// always with the painted check mark) gets its colour here: hint 4.5:1 toward the background, error red and ok green,
// each replaced by the window text colour when it would read below 4.5:1 on the window. Meaning never rests on colour
// (every state has words). Style sheets are built with fromLatin1 (TeamSpeak's QStyleSheetStyle keeps the parsed text
// after an unload), scoped to the given object name.

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QPixmap>
#include <QString>

class QComboBox;
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

// ---- 2.7 F3: the new Settings window's colour tokens and size factor (design_settings 3.3, design H1) ----------------
// Every colour of the cards, switches, tiles, the sidebar and the painted icons comes from the dialog's polished palette
// (TeamSpeak's skins put their colours there) through these tokens, with contrast floors: secondary text 4.6:1 on card,
// window and sidebar; disabled text, the switch outline, the accent and the focus ring 3:1. k is the font-based size
// factor: TeamSpeak's 8.25 pt font is 11 px at 100 % and 22 px at 200 %, and TeamSpeak doesn't scale widgets itself, so
// every pixel size of the window is px(v) = round(v * k) (1 px borders and dividers stay 1 px).
struct Tokens {
    bool   dark = false;
    qreal  k    = 1.0;
    QColor window, text, card, border, divider, sidebar, muted, disabled, control, accent, onAccent, focus, navSel, navHover;

    int px(qreal v) const { return qRound(v * k); }
};

QColor mix(const QColor& a, const QColor& b, double t);
// A brush's colour: its own when solid, the average of a gradient's stops, else fallback (a picture).
QColor solidOf(const QBrush& brush, const QColor& fallback);
// The font's height in pixels (exact for a point-sized font at the widget's DPI) over 11, at least 1.
qreal  sizeFactor(const QFont& font, const QWidget* widget);
// The tokens for a palette and the skin's push-button colour (an invalid colour: none).
Tokens tokens(const QPalette& palette, const QColor& skinButton, qreal k);

// The Settings dialog keeps its tokens (computed once per theme change); its painted widgets ask for them here.
class TokenSource
{
  public:
    virtual ~TokenSource()                    = default;
    virtual const Tokens& themeTokens() const = 0;
    virtual bool          keyboardFocusShown() const = 0; // focus rings only after the keyboard was used
};
// The tokens of the window w is in: a TokenSource's (the Settings dialog), else computed from that window's palette and
// font (tests, a section shown on its own).
Tokens tokensFor(const QWidget* w);
// w has the focus and its window shows focus rings (TokenSource: once the keyboard was used; any other window: always).
bool   showsFocus(const QWidget* w);
// Live test 2.7 (F3-1): the width a combo box needs so that its longest item isn't cut: its size hint, made wider by
// what the style's own edit field lacks for that item (styles and style sheets each draw the arrow and frame their own
// way; at 200 % the Windows style's arrow grows more than the size hint allows for). Editable or empty: the size hint.
int    comboFitWidth(const QComboBox* combo);

} // namespace settingstheme
