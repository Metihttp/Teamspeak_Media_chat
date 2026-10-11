#pragma once

// Chat redesign: the look of TS Media's chat layout (layoutdoc.h): its size tokens for the chat font, its
// colours for TeamSpeak's skins (contrast-checked), and every picture of it (message heads with an
// avatar or initials, the Discord-style reply row, compact heads, system icons, dividers, chips, the
// gutter time) plus the action bar's glyphs. QtGui only: render_gallery and the tests draw with it too.
// Sizes are logical pixels; pictures carry their device pixel ratio.

#include <QColor>
#include <QDate>
#include <QFont>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include "replyart.h"

class QPainter;

namespace layoutart {

enum class Mode { Classic = 0, Cozy = 1, Compact = 2 };

// ---- tokens -------------------------------------------------------------------------------------------
// f is the pixel size of the chat's header font (TeamSpeak's 9 pt is 12 px); the comments give f = 12.
struct Tokens {
    Mode  mode = Mode::Cozy;
    int   f    = 12;
    bool  ampm = false; // TeamSpeak's headers show "4:00:17 PM"
    // cozy
    int G         = 48; // the content column
    int avatar    = 24;
    int avatarX   = 8;
    int headRow   = 24;
    int replyRow  = 18;
    int lineMin   = 17; // body lines: at least this high (MinimumHeight)
    int headTop   = 14; // a group's first message
    int afterDivider = 6;
    int systemTop    = 8; // the first of a run of system rows
    int systemRunTop = 2; // the others
    int dividerTop    = 18;
    int dividerHeight = 20;
    int dividerBottom = 2;
    int nameGap       = 8; // name to time
    // compact
    int L                = 38; // the time column
    int compactLeading   = 2;  // LineDistanceHeight
    int compactHeadTop   = 4;
    int compactSystemTop = 2;
    int compactNameGap   = 7;
    // fonts
    QFont body;     // TeamSpeak's (never changed)
    QFont name;     // f, DemiBold
    QFont time;     // 0.84 f (at least 10 px)
    QFont compactTime; // 0.92 f
    QFont divider;  // 0.88 f, DemiBold
    QFont reply;    // 0.92 f
    QFont chip;     // 0.84 f

    int indent() const { return mode == Mode::Compact ? L : G; }
};
// chatFont: the header's font (resolved). ampm: the headers' times have AM/PM.
Tokens tokensFor(Mode mode, const QFont& chatFont, bool ampm);

// ---- colours ------------------------------------------------------------------------------------------
struct Colors {
    bool   dark = true;
    QColor base;       // the chat's background
    QColor text;       // TeamSpeak's body text
    QColor name;       // names
    QColor muted;      // times, system rows, divider labels
    QColor mutedOnMention; // muted text on a row that mentions you (dark: a step lighter, 4.5:1)
    QColor link;       // links (the skin's, nudged to 4.5:1)
    QColor linkUnderline; // link at 40 %
    QColor hover;      // the hovered row (translucent; painted under the text)
    QColor mentionTint;
    QColor mentionBar;
    QColor pillBack;   // translucent
    QColor pillText;
    QColor divider;    // hairline (decorative)
    QColor focusBar;
    QColor accent;
    QColor join;
    QColor leave;      // leave, kick, ban, error
    QColor announce;   // poke, welcome
    QColor danger;     // kick, ban and error text (TeamSpeak's red at 4.5:1)
    QColor spine;      // the reply row's connector
    QColor chipBorder; // muted at 40 %
    QColor chipHover;  // text at 8 %
    // the action bar
    QColor barBack, barBorder, barIcon, barHoverBack, barHoverIcon, barPressed, barShadow;
};
// skinLink: the skin's link colour (invalid: none known); skinText: the viewport's Text.
Colors colorsFor(bool dark, const QColor& base, const QColor& skinText, const QColor& skinLink);
// A nickname's colour as TeamSpeak showed it: the usual one becomes the name colour; another (the skin's
// friend / blocked markings) keeps its hue at 4.5:1.
QColor nameColorFor(const Colors& colors, const QColor& nickColor, const QColor& usualNickColor);
// The flat colour of the hover / mention row (for contrast checks).
QColor hoverRow(const Colors& colors);
QColor mentionRow(const Colors& colors);
// Every text / icon pair of colors with its contrast and the minimum it needs (the tests check them).
struct Pair {
    QString what;
    QColor  fore;
    QColor  back;
    double  minimum;
};
QVector<Pair> contrastPairs(const Colors& colors);

// ---- labels -------------------------------------------------------------------------------------------
// "16:00" / "4:00 PM" from TeamSpeak's "16:00:17" / "4:00:17 PM" (empty if it isn't a time).
QString shortTime(const QString& headerTime, bool ampm);
// Seconds of the day of "16:00:17" / "4:00:17 PM"; -1 if it isn't a time.
int secondsOf(const QString& headerTime);
bool hasAmPm(const QString& headerTime);
// "Today at 16:00", "Yesterday at 22:34", "10/8/2026 16:00", or the time alone without a date.
QString headTimeLabel(const QDate& date, const QDate& today, const QString& headerTime, bool ampm);
// "Saturday, October 10, 2026 16:00:17" (English); the time alone without a date.
QString fullTimeLabel(const QDate& date, const QString& headerTime);
// "Today, October 10, 2026", "Yesterday, October 9, 2026", "Monday, September 28, 2026".
QString dayLabel(const QDate& date, const QDate& today);
// "History · April 11, 2026" (or "History" without a date).
QString historyLabel(const QDate& date);
// A TeamSpeak day line's date ("9/28/2026"): the system's short format first, then M/d/yyyy, d.M.yyyy,
// yyyy-MM-dd and d/M/yyyy. Invalid if none reads it.
QDate parseDayText(const QString& text);

// ---- pictures -----------------------------------------------------------------------------------------
QColor initialsColor(const QString& uid); // one of 8, white initials keep 5:1 on each
// A round avatar: the picture (cropped to a circle) or initials of nick on initialsColor(uid).
QImage avatarImage(const QString& nick, const QString& uid, const QImage& picture, int px, qreal dpr);

struct HeadSpec {
    QString nick;
    QString uid;
    QColor  nameColor;
    QString timeLabel;   // empty: none
    QImage  avatar;      // null: initials
    bool    showAvatar = true;
    // a reply head: the reply row above (Discord's order)
    bool             reply = false;
    replyart::Header replyHeader;
    QColor           replyName; // the original's name colour (replyart::headerNameColor)
    QImage           replyAvatar;
    bool             replyHover   = false;
    bool             replyPressed = false;
};
struct HeadGeometry {
    QRectF  avatar;
    QRectF  name;
    QRectF  time;     // empty: dropped (too narrow)
    QRectF  replyRow; // the click target of a reply head (empty otherwise)
    QString shownName;
    bool    elided = false;
    qreal   baseline = 0;
};
// The head picture's size: as wide as name and time (or the reply row), at most maxWidth.
QSize        headSize(const HeadSpec& spec, const Tokens& tokens, int maxWidth);
HeadGeometry headGeometry(const HeadSpec& spec, const Tokens& tokens, const QSize& size);
QImage       renderHead(const HeadSpec& spec, const Tokens& tokens, const Colors& colors, const QSize& size, qreal dpr);

// Compact: [time in the L column][name][gap], as high as the body font's ascent + descent (baseline-aligned).
// nameOnly: a compact reply's head (the time is painted in the gutter).
QSize  compactHeadSize(const QString& nick, const Tokens& tokens, bool nameOnly, int maxWidth);
QImage renderCompactHead(const QString& nick, const QColor& nameColor, const QString& time, const Tokens& tokens, const Colors& colors, const QSize& size,
                         bool nameOnly, qreal dpr);
qreal  bodyAscent(const Tokens& tokens);

// System rows.
enum class SystemKind { Info, Join, Leave, Danger, Group, Edit, Announce, Poke, TsMedia, Warning };
QColor systemIconColor(SystemKind kind, const Colors& colors);
void   drawSystemIcon(QPainter& p, SystemKind kind, const QRectF& box, const QColor& color);
// Cozy: G wide, lineMin high, the icon centred at the avatar's x; compact: L + 20 wide with the time first.
QSize  systemPrefixSize(const Tokens& tokens);
QImage renderSystemPrefix(SystemKind kind, const QString& time, const Tokens& tokens, const Colors& colors, qreal dpr);

// Dividers: a hairline, the centred label, the hairline again.
QImage renderDivider(const QString& label, int width, const Tokens& tokens, const Colors& colors, qreal dpr);
int    dividerHeight(const Tokens& tokens);

// Chips ("×3", "+5 more events", "Show fewer").
QSize  chipSize(const QString& text, const Tokens& tokens);
QImage renderChip(const QString& text, bool hovered, const Tokens& tokens, const Colors& colors, qreal dpr);

// The time painted in the gutter (a hovered continuation, a compact reply head): right-aligned at right,
// on baseline.
void paintGutterTime(QPainter& p, const QString& text, qreal right, qreal baseline, const Tokens& tokens, const Colors& colors, bool compact);

// ---- the action bar ------------------------------------------------------------------------------------
enum class Glyph { React, Reply, Copy, More, Check };
void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color);

} // namespace layoutart
