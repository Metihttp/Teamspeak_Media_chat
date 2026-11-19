#pragma once

// 2.7 Dark mode: the look (design_dark 2): the colour tokens of the warm dark theme, the application palette, the
// application style sheet and the chat document style sheet, built at run time from templates. Pure (QtGui only, no
// widget is touched): DarkMode (darkmode.h) applies them.
//
// CRASH RULE: TeamSpeak's QStyleSheetStyle and every QTextDocument keep the text they were given after the plugin is
// unloaded, so nothing built here shares data with the DLL image: the templates are const char[] read through
// QString::fromLatin1, and every replacement is built with QString::number / arg / fromLatin1 (never QStringLiteral).

#include <QColor>
#include <QPair>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QVector>

#include "settingstheme.h"

class QFont;

namespace darkpalette {

// Every colour of the theme (design_dark 2.1). selection and selectionInactive are translucent (alpha 77 and 41).
struct Tokens {
    QColor panel, window, raised, field, raisedHover, raisedPressed, hover, menuHover, hoverStrong, border, control, controlHover;
    QColor text, secondary, muted, disabled, disabledFill;
    QColor accent, accentHover, accentPressed, onAccent, accentDeep, accentOff, onAccentOff, accentTint;
    QColor link, selection, selectionInactive, thumb, thumbHover, thumbPressed, alt, success, warning, error, handleHover;
};
const Tokens& tokens();

// The Settings window's tokens under dark mode (design_dark 1.11), at size factor k.
settingstheme::Tokens settingsTokens(qreal k);

// The application palette under dark mode (design_dark 2.8): base with the theme's roles set in every group. Never
// LinkVisited (a role outside the skin's resolve mask is not given back by setPalette, design_dark 1.3).
QPalette palette(const QPalette& base);

// The skin's chat geometry the dark sheet keeps (design_dark 1.5, 2.6): the ChatTab's frame width and the width of its
// vertical scroll bar. 2.7.2 (D3): and the side insets of the chat tab widget's pane (where the skin puts its pages), so
// the chat column keeps its width (a hidden chat laid out again at its Show for a column a few px wider or narrower showed
// a horizontal scroll bar or moved bubbles for ~200 ms). paneSelector names that tab widget in a sheet (its class, or
// QTabWidget#name); empty: nothing measured, the design's pane. paneProbed (review fix): measured on probes because no chat
// existed yet (TeamSpeak's start with dark mode on), the selector TeamSpeak's chat tab widget class until a chat shows its own.
// 2.7.3 (M6): and the chat column's height (dark mode's bars grew with k, TeamSpeak's grow with the font only: the chat was
// 18 px shorter at 100 %, 60-73 px at 200 %): the pane's top and bottom insets (paneTop, paneBottom; -1: not measured), and
// the heights of TeamSpeak's main window bars in the skin (0: not measured or not there: the design's): a menu bar item and
// the menu bar font's line (and the menu bar's frame and margin around its items), the tool bar, its tallest button and that button's contents (its icon), a tab of the server
// tabs and of the chat tabs, the tool buttons beside the chat's input (TeamSpeak's emoticon button) and their contents, the
// splitters' handles and the status bar. barsProbed: measured on a probe window under the skin's sheet (TeamSpeak's start
// with dark mode on, before it built its window); the first chat's window gives the real contents (DarkMode::confirmPane).
// Review fix (2.7.3): areas, the smallest heights of the main splitter's named areas (TeamSpeak's server area and chat area)
// that have no minimum of their own: dark mode's scroll bars and frames gave them other ones, and an area at its smallest
// (the splitter dragged to an end) moved the chat at every switch.
struct Metrics {
    int     chatFrame = 1;
    int     chatBar   = 17;
    QString paneSelector;
    int     paneLeft   = 0;
    int     paneRight  = 0;
    bool    paneProbed = false;
    int     paneTop      = -1;
    int     paneBottom   = -1;
    int     menuItem     = 0;
    int     menuText     = 0;
    int     menuMargin   = 0;
    int     toolBar      = 0;
    int     toolButton   = 0;
    int     toolContent  = 0;
    int     serverTab    = 0;
    int     chatTab      = 0;
    int     inputButton  = 0;
    int     inputContent = 0;
    int     splitterV    = 0;
    int     splitterH    = 0;
    int     status       = 0;
    bool    barsProbed   = false;
    QVector<QPair<QString, int>> areas;
};

// 2.7.2 (D3): the rule that gives the chat tab widget's pane the skin's side insets in the window colour (seamless: the
// pages' background is the same colour); empty without a selector. 2.7.3 (M6): and its top and bottom insets when measured
// (the design's line along the top kept inside the top inset). Built at run time (CRASH RULE).
QString paneRule(const Tokens& t, const Metrics& m);
// 2.7.3 (M6): the rules that give TeamSpeak's main window bars the skin's heights (vertical paddings and min/max heights;
// the horizontal paddings, colours, radii and accents stay the design's), at size factor k; one per measured bar, empty
// when none was. The main window's own bars only (QMainWindow > its menu bar, tool bars, status bar; the two named tab
// areas; the tool buttons beside the chat's input): dialogs keep the design's. Review fix: and the main splitter's areas
// their smallest heights (Metrics::areas). Built at run time (CRASH RULE).
QString barRules(qreal k, const Metrics& m);

// The size factor of design_dark 2.4: the font's pixel size over 11 (TeamSpeak's 8.25 pt at 100 %), at least 1.
qreal scaleFactor(const QFont& font);
// round(v * k), the template's {N}.
int px(qreal v, qreal k);

// A token as the sheets write it: "#rrggbb", or "rgba(r, g, b, a)" for a translucent one.
QString cssColor(const QColor& color);
// The token's colour by its template name ("panel", "accent", ...); invalid: no such token.
QColor tokenByName(const Tokens& t, const QString& name);

// The application style sheet (design_dark 2.6) for size factor k, the skin's chat metrics and the folder of the glyph
// pictures (darkglyphs.h). An empty glyphDir drops every rule that shows a glyph (the native indicators show then).
// unknown (tests): the names of tokens the template asked for that don't exist (their rules are left out).
QString appSheet(const Tokens& t, qreal k, const Metrics& m, const QString& glyphDir, QStringList* unknown = nullptr);
// The chat document style sheet for new lines (design_dark 2.7), the template as written (its own spacing: TeamSpeak's).
QString chatSheet(const Tokens& t, QStringList* unknown = nullptr);
// 2.7.3 (M9): the one dark mode gives a document whose own sheet is skinChat (its palette(...) resolved): the template's
// colours and text styles without any of its spacing, then skinChat's own spacing declarations (margins, paddings, table
// spacing, vertical alignment, line height, indent, text alignment, white space, the font's size and family and the font
// shorthand), each with its selector. Lines printed while dark mode is on are placed exactly as the style places them.
QString chatSheet(const Tokens& t, const QString& skinChat, QStringList* unknown = nullptr);
// Every font declaration (font, font-*) of a skin's application sheet, each rule with its selector as the skin wrote it
// (QLabel#OptionsHeadlineTitleLabel { font: 18px "TeamSpeak 2020"; }), nothing else. Dark mode puts them after its own
// sheet (review fix): the dark sheet has no font of its own, and replacing the skin's sheet would otherwise drop the fonts
// the skin gives TeamSpeak's headlines, banners and, in a custom skin, the whole client or the chat.
QString fontRules(const QString& sheet);

// The templates as written (tests: the placeholders, the size budget).
QString appSheetTemplate();
QString chatSheetTemplate();

} // namespace darkpalette
