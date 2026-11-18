#pragma once

// 2.7 Dark mode: the colours already baked into TeamSpeak's chat documents (design_dark 1.5). TeamSpeak's document style
// sheet turns its classes into character formats when a line is inserted, so a new sheet only changes new lines. The
// lines already there are recoloured by "look" (colour, bold, underline, and since 2.7.1 whether it is a link): the maps
// are computed at run time from the two document sheets (a scratch document per class, the parser TeamSpeak's insertions
// use), so any skin's sheet works with no colour table here. Every recoloured fragment keeps its colour before in
// kOriginal, so the way back is exact; lines added while dark (no kOriginal) get the skin's colour of their class back,
// and since 2.7.2 its text style too (weight, italic, underline, size: where the two sheets differ for a look, and every
// class of that look agrees on the skin's value). The info panel has maps of its own (its classes first; a class the skin
// doesn't colour loses dark mode's colour on the way back). kOriginal never survives the way back: it is also taken off
// what copied a recoloured fragment's format (an HD emoji's picture, the chat layout's recoloured fragment, the block
// separators QTextEdit::append copies from its cursor, the chat cursor's own format: DarkMode).
// TeamSpeak's names, channels and info panel titles are links (its binary and templates put those classes on <a> only);
// TS Media's own lines (the bold "TS Media chat" label, its text) never are, so a nickname and TS Media's bold text, both
// "the chat text, bold" under dark mode, are told apart (2.7.1), and the label has a map of its own (isLineLabel) for the
// one look it shares with TS Media's text (Same as the chat text). QtGui only (the tests run it as it is).
//
// Never recoloured: pictures, objects (spoiler bars, previews, poll cards), and the colours TS Media's own modules set
// (a property in 0x7450-0x74ff: the chat layout's pictures and the colours it sets itself, replies, emoji, text spoilers,
// reactions, polls): they take their colours from the chat's palette themselves. A link the chat layout only gave its
// underline colour keeps TeamSpeak's colour, which is recoloured; so do an album's collapsed item link (2.7.1: its text
// is kept as a string, its colour is TeamSpeak's link colour), a text spoiler's revealed text and a link's space in a hidden
// one, and the reply line's and the poll card's line breaks. What the modules keep of TeamSpeak's text to give it
// back later (review fix: the chat layout's header runs and its recoloured fragments' formats before, an HD emoji's text
// format, a text spoiler's hidden runs, a reply's quote line, a poll card's text) is mapped the same way inside their
// formats, both ways, with kOriginal in there too: switching the layout to Classic, turning a module off or unloading
// gives back TeamSpeak's text in the colours of the side dark mode is on, and nothing of the other side stays behind.

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QTextFormat>
#include <QVariant>
#include <QVector>

class QPalette;
class QTextCharFormat;
class QTextDocument;

namespace darkchat {

// The colour a fragment had before dark mode (uint ARGB). An int property: it holds no string of this DLL.
constexpr int kOriginal = QTextFormat::UserProperty + 0x0D41;

struct Look {
    QRgb rgb       = 0;
    bool bold      = false; // font weight above Normal
    bool underline = false;
    bool anchor    = false; // 2.7.1: inside a link (TeamSpeak's names, channels and info titles are; TS Media's line text isn't)
    bool operator==(const Look& o) const { return rgb == o.rgb && bold == o.bold && underline == o.underline && anchor == o.anchor; }
};
uint qHash(const Look& look, uint seed = 0);
Look lookOf(const QTextCharFormat& format);

// The look of TeamSpeak's chat and info panel classes under css, in priority order (the first class wins a look two
// classes share). plainLink: the application palette's link colour that goes with css (Qt gives a link the palette's
// colour when the sheet doesn't colour it).
QVector<QPair<QByteArray, Look>> classLooks(const QString& css, const QColor& plainLink);
// 2.7.2: every class of classLooks' list (also the ones css doesn't colour, and the info panel's labels, custom nickname,
// title text and bottom cells) with its whole format under css: the probe's text fragment (a link without a colour from
// css: plainLink's). colored: css gives it a solid colour (look is valid then). classLooks is the coloured part of this list.
struct ClassFormat {
    QByteArray      key;
    bool            colored = false;
    Look            look;
    QTextCharFormat format;
};
QVector<ClassFormat> classFormats(const QString& css, const QColor& plainLink);
// css with every palette(role) reference (TeamSpeak's sheets use palette(link)) replaced by that role's colour in palette:
// the colour the line got when it was inserted, whatever the application palette is when the looks are computed.
QString resolvePaletteRefs(const QString& css, const QPalette& palette);

// 2.7.2 (D5): one property of a class's text style on the way back: id gets value (invalid: none) on a fragment whose value
// is one of from (the values the classes of its look have on the side it comes from; invalid: none). Any other value is
// someone else's (a line's own [b], an old line): left as it is.
struct StyleFix {
    int          id = 0;
    QVariant     value;
    QVariantList from;
};
// The look of a fragment without a colour of its own (2.7.2: the info panel title's text and pictures): never a real colour's.
Look uncoloredLook(const QTextCharFormat& format);

struct ColorMap {
    QHash<Look, QRgb> exact;        // a known look -> its colour on the other side
    QHash<Look, QRgb> label;        // 2.7.1, the way back: TS Media's line label (isLineLabel), looked up before exact
    QColor            base;         // the chat's background on the target side (forward: anything else under minimum is lifted)
    double            minimum = 4.5;
    // 2.7.2, the way back only. uncolored: a look of an info panel class the target sheet doesn't colour (its colour goes,
    // TeamSpeak's panel shows its text colour there). fonts: for a look a class won, the properties other than the colour
    // where the two sheets differ and every class of that look agrees on the target's value (StyleFix); in the info panel's
    // map also for the uncolored look (uncoloredLook) of its title's text, which neither side colours.
    QSet<Look>                     uncolored;
    QHash<Look, QVector<StyleFix>> fonts;
};
// from -> to for every class both sides colour (the first class wins a look); extras only fill looks the chat classes don't
// have, and the info panel's classes (InfoTitleLink, Highlight, Important, Active, Inactive) only what is still free.
// labels: the label map (the first entry wins a look). No fonts (2.7.1's maps; tests).
ColorMap buildMap(const QVector<QPair<QByteArray, Look>>& from, const QVector<QPair<QByteArray, Look>>& to, const QVector<QPair<Look, QRgb>>& extras,
                  const QColor& base, const QVector<QPair<Look, QRgb>>& labels = {});
// 2.7.2: the same from whole class formats, with the fonts. infoPanel false (a chat): the colours exactly as above (the info
// panel's Label and CustomNick take no part). infoPanel true (TeamSpeak's info panel): its classes win their looks first,
// then the chat classes, then the extras; an info panel class the target doesn't colour goes to uncolored.
ColorMap buildMap(const QVector<ClassFormat>& from, const QVector<ClassFormat>& to, const QVector<QPair<Look, QRgb>>& extras, const QColor& base,
                  const QVector<QPair<Look, QRgb>>& labels = {}, bool infoPanel = false);
// 2.7.1: text is TS Media's line label: exactly "TS Media chat" or "TS Media chat" + a space + U+26A0 (ts3api's printInfo / printWarning;
// layoutdoc reads the same label). Not the chat layout's notice "TS Media chat:" (printed as text).
bool isLineLabel(const QString& text);

// A fragment of TS Media's modules (a property in 0x7450-0x74ff).
bool isModuleFormat(const QTextCharFormat& format);

// Recolours [from, to) of doc (to < 0: the end) in one edit block. Forward: a solid colour with a known look gets the
// map's colour, any other one under map.minimum on map.base is lifted (hue kept); each keeps its colour before in
// kOriginal; fragments that carry kOriginal already are left. Back: kOriginal's colour, else a known look's colour (lines
// added while dark; TS Media's label from map.label first). The same for every format a module keeps inside a fragment's
// format (a run kept with its text is the label when that text is). Returns the number of fragments changed.
// 2.7.2, the way back: the look's text style (map.fonts) with its colour, an info panel class's colour taken off
// (map.uncolored), and every block's own character format in the range mapped like a fragment (QTextEdit::append copies its
// cursor's format into each new block's separator).
int recolor(QTextDocument* doc, const ColorMap& map, bool forward, int from = 0, int to = -1);
// 2.7.2: one format on the way back, as recolor() maps a fragment's (the chat cursor's own format). False: unchanged.
bool mapBack(QTextCharFormat& format, const ColorMap& map);
// How many fragments carry kOriginal (tests, diagnostics).
int taggedCount(QTextDocument* doc);
// Goes up whenever recolor() changed a colour a module keeps: a module that reads those for its statistics (the chat
// layout's usual nickname and link colours) reads them again. GUI thread.
quint64 keptSerial();

// The tool tip map (design_dark 1.6): TeamSpeak's template colours blue -> the link colour, red and dark red -> the error
// colour, anything else under 4.5:1 on the tip's background lifted.
ColorMap tipMap(const QColor& link, const QColor& error, const QColor& tipBase);

} // namespace darkchat
