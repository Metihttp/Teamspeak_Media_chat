#pragma once

// 2.7 Dark mode: the colours already baked into TeamSpeak's chat documents (design_dark 1.5). TeamSpeak's document style
// sheet turns its classes into character formats when a line is inserted, so a new sheet only changes new lines. The
// lines already there are recoloured by "look" (colour, bold, underline, and since 2.7.1 whether it is a link): the maps
// are computed at run time from the two document sheets (a scratch document per class, the parser TeamSpeak's insertions
// use), so any skin's sheet works with no colour table here. Every recoloured fragment keeps its colour before in
// kOriginal, so the way back is exact; lines added while dark (no kOriginal) get the skin's colour of their class back.
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
#include <QTextFormat>
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
// css with every palette(role) reference (TeamSpeak's sheets use palette(link)) replaced by that role's colour in palette:
// the colour the line got when it was inserted, whatever the application palette is when the looks are computed.
QString resolvePaletteRefs(const QString& css, const QPalette& palette);

struct ColorMap {
    QHash<Look, QRgb> exact;        // a known look -> its colour on the other side
    QHash<Look, QRgb> label;        // 2.7.1, the way back: TS Media's line label (isLineLabel), looked up before exact
    QColor            base;         // the chat's background on the target side (forward: anything else under minimum is lifted)
    double            minimum = 4.5;
};
// from -> to for every class both sides colour (the first class wins a look); extras only fill looks the chat classes don't
// have, and the info panel's classes (InfoTitleLink, Highlight, Important, Active, Inactive) only what is still free.
// labels: the label map (the first entry wins a look).
ColorMap buildMap(const QVector<QPair<QByteArray, Look>>& from, const QVector<QPair<QByteArray, Look>>& to, const QVector<QPair<Look, QRgb>>& extras,
                  const QColor& base, const QVector<QPair<Look, QRgb>>& labels = {});
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
int recolor(QTextDocument* doc, const ColorMap& map, bool forward, int from = 0, int to = -1);
// How many fragments carry kOriginal (tests, diagnostics).
int taggedCount(QTextDocument* doc);
// Goes up whenever recolor() changed a colour a module keeps: a module that reads those for its statistics (the chat
// layout's usual nickname and link colours) reads them again. GUI thread.
quint64 keptSerial();

// The tool tip map (design_dark 1.6): TeamSpeak's template colours blue -> the link colour, red and dark red -> the error
// colour, anything else under 4.5:1 on the tip's background lifted.
ColorMap tipMap(const QColor& link, const QColor& error, const QColor& tipBase);

} // namespace darkchat
