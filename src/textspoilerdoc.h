#pragma once

// 2.7 F1: text spoilers in TeamSpeak's chat documents (QtGui only, like replydoc.h and polldoc.h: the tests run it on
// documents built like TeamSpeak's). ChatTextSpoilers (chattextspoilers.h) decides when; this changes the document.
//
// A spoiler "[spoiler]the text[/spoiler]" (textspoiler.h) in a message's text is put into the document as custom text
// objects of ours (not pictures: Ctrl + wheel, the chat font and the skin need no new drawing, and the message's text
// for every reader of it stays the same):
//  * Hidden: the opening and the closing marker each become one object of no size; each word of the text (a run of
//    non-space characters; a longer one in pieces of 12 so the bars can wrap) becomes one object drawn as a rounded bar
//    as wide as the word; spaces and line breaks stay text (a space of a link loses its link). The text itself is not in
//    the document any more: no link, no preview, no mention, no copy, no selection shows it.
//  * Revealed: the markers stay objects of no size; the text is text again, tinted.
// Every object keeps its pieces as TeamSpeak showed them (kRuns: text and format pairs; an HD emoji as its text, a
// TeamSpeak emoticon as its picture, a recoloured fragment with its format before) and what replydoc::parseBlock reads
// for them (kIdentity), so a message's text, its text-reaction key and its link-preview identity are the same hidden,
// revealed and raw. Every object's format is unique (kId + kPiece): two equal formats would make one fragment.
// restoreAll() gives TeamSpeak's text back exactly, fragment for fragment (unload, tests).
//
// Not a spoiler (left as typed): a pair whose text holds anything but TeamSpeak's text (a TS Media file link, one of our
// pictures, rows, cards or line breaks, a revealed spoiler), text before the message's text start (its header, a
// reply's quote line), an opening marker without a closing one, an empty pair.

#include <QColor>
#include <QHash>
#include <QObject>
#include <QSizeF>
#include <QString>
#include <QTextFormat>
#include <QTextObjectInterface>
#include <QVector>

class QTextBlock;
class QTextCharFormat;
class QTextDocument;

namespace textspoilerdoc {

// Free in every chat document of ours: 0x7490-0x749f (other modules: 0x7453-0x748a, 0x74a0-0x74e0).
constexpr int kObjectType = QTextFormat::UserObject + 0x7490;   // our objects' type
constexpr int kKind       = QTextFormat::UserProperty + 0x7490; // int: Kind
constexpr int kId         = QTextFormat::UserProperty + 0x7491; // int: the spoiler's serial in its document
constexpr int kRuns       = QTextFormat::UserProperty + 0x7492; // QVariantList: text, QTextFormat, ... as TeamSpeak showed it
constexpr int kIdentity   = QTextFormat::UserProperty + 0x7493; // QString: what replydoc::parseBlock reads for it
constexpr int kBefore     = QTextFormat::UserProperty + 0x7494; // QTextFormat: a tinted or unlinked fragment's format before
constexpr int kKey        = QTextFormat::UserProperty + 0x7495; // QString on the opening marker: message hash + ":" + n
constexpr int kPiece      = QTextFormat::UserProperty + 0x7496; // int: the object's index in its spoiler (formats unique)
constexpr int kState      = QTextFormat::UserProperty + 0x7497; // int on both markers: 0 hidden, 1 revealed
constexpr int kMaxPiece   = 12;                                 // characters per word bar at most

enum Kind { None = 0, Open = 1, Close = 2, Word = 3, Revealed = 4, Space = 5 };

bool    isObject(const QTextFormat& format); // one of our objects (markers and words)
int     kindOf(const QTextFormat& format);   // Kind (objects and our text fragments), None: not ours
QString identityText(const QTextFormat& format);

// A raw pair in the document (positions).
struct Pair {
    int open         = 0;
    int contentStart = 0;
    int contentEnd   = 0;
    int closeEnd     = 0;
};
// The raw pairs of block's text from position from (the message's text start), in order.
QVector<Pair> rawPairs(const QTextBlock& block, int from);

// Hides pairs (of one block, in order; done back to front) inside one edit block. keys: each pair's key (kKey);
// revealed: put straight into the revealed state. *serial: the document's next spoiler id (in and out). The caller
// keeps ChatIntegration's "mutating" flag set. Returns how many pairs changed.
int hide(QTextDocument* doc, const QVector<Pair>& pairs, const QVector<QString>& keys, const QVector<bool>& revealed, int* serial);

// One spoiler of a document, found by its opening marker.
struct Spoiler {
    int     id       = -1;
    int     open     = -1; // the opening marker's position
    int     close    = -1; // the closing marker's position
    bool    revealed = false;
    QString key;
    int     block = -1;
};
// Every spoiler of block (all blocks when block is invalid).
QVector<Spoiler> spoilersIn(QTextDocument* doc, int blockNumber = -1);
// The spoiler id of our word object at position (-1: none there).
int wordIdAt(QTextDocument* doc, int position);
// A hidden spoiler revealed: its words' text back, tinted. False if it isn't hidden.
bool reveal(QTextDocument* doc, const Spoiler& spoiler);
// A spoiler (hidden or revealed) back to its raw text (to hide it again).
bool unhide(QTextDocument* doc, const Spoiler& spoiler);
// Something of ours between the spoiler's markers that isn't its text (a link preview's slot under a revealed link).
bool holdsOthers(QTextDocument* doc, const Spoiler& spoiler);
// Everything of ours given back (one edit block). Returns how many spoilers.
int restoreAll(QTextDocument* doc);

// How the bars look in a document (ChatTextSpoilers keeps it up to date; the handler only reads it).
struct Look {
    QColor base;          // the chat's background
    QColor text;          // its text
    int    hovered = -1;  // the spoiler under the pointer
    int    pressed = -1;  // ... pressed
};
// The bar's colour: 50 % of the way from base to text, 60 % hovered, 70 % pressed.
QColor barColor(const Look& look, int id);
// A word object's size for doc's fonts now (markers: none).
QSizeF objectSize(const QTextDocument* doc, const QTextFormat& format);

// Lays out and draws our objects (registered on every chat document ChatTextSpoilers attaches to). Q_INTERFACES gives
// Qt the interface without a metatype.
class Handler : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

  public:
    explicit Handler(QObject* parent = nullptr);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void   drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument, const QTextFormat& format) override;

    void setLook(const QTextDocument* doc, const Look& look);
    Look look(const QTextDocument* doc) const;
    void forget(const QTextDocument* doc);

  private:
    QHash<const QTextDocument*, Look> m_looks;
};

} // namespace textspoilerdoc
