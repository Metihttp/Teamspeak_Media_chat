#pragma once

// 2.4 link previews in TeamSpeak's chat document. QtGui only (no widgets): the tests and render_gallery run it
// on documents built like TeamSpeak's (tests/tschat.h).
//
// Which links: the web anchors TeamSpeak made in chat messages (it links typed addresses; [URL] BBCode too)
// that linkpreview::classify() takes (YouTube videos, also http ones, as TeamSpeak links an address typed
// without a scheme; https picture / GIF / video files). Never in TS Media's own
// lines, never after a TS Media file link in its message (the "plugin required" note and its link), never the
// plugin's own addresses (the caller passes them), at most kMaxPerMessage per message, one per address.
//
// The slot: right after the link, our line break (U+2028) and one picture, then another line break of ours
// when text follows on that line. The picture is either a card of ours ("tsmedia-link:<n>", drawn by
// ChatLinkPreviews) or a media preview ("tsmedia:<key>", ChatIntegration's own scheme, drawn and played by it
// like uploaded media). Both carry our properties (0x74d0-0x74d3; other modules use 0x7453-0x74c2), so they
// are found by property, never by a remembered position, and every other module tells them apart (text
// reactions read the message as if they weren't there). Blocks are never added or removed; removeAll gives
// TeamSpeak's document back exactly (scrubbing what TeamSpeak copied of our formats into the next line).
// Everything read from a chat is untrusted: only checked addresses come out of it, nothing is fetched here.

#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTextFormat>
#include <QVector>

#include "linkpreview/linkurl.h"

class QTextBlock;
class QTextCharFormat;
class QTextDocument;

namespace linkdoc {

constexpr int kSlotObject    = QTextFormat::UserProperty + 0x74d0; // bool: our picture (a card or a media preview)
constexpr int kSlotUrl       = QTextFormat::UserProperty + 0x74d1; // QString: the link's Target::key
constexpr int kSlotSeparator = QTextFormat::UserProperty + 0x74d2; // bool: our line break around it
constexpr int kSlotHref      = QTextFormat::UserProperty + 0x74d3; // QString: the link as TeamSpeak had it
constexpr int kMaxBlocks     = 4000; // the newest blocks of a chat that are read
constexpr int kMaxPerMessage = 3;
constexpr int kMaxHrefChars  = 2048;

QString cardPrefix();  // "tsmedia-link:"
QString mediaPrefix(); // "tsmedia:" (ChatIntegration's)

bool isSlotObject(const QTextCharFormat& format);
bool isSlotSeparator(const QTextCharFormat& format);
bool carriesSlot(const QTextFormat& format); // any property of ours (also on text TeamSpeak copied)

// ---- reading ------------------------------------------------------------------------------------------

struct Link {
    int                 block = -1;
    int                 start = 0; // the anchor: [start, end) (one link split over several fragments is one)
    int                 end   = 0;
    QString             href;
    linkpreview::Target target;
    bool                outgoing = false; // your message
};
// excluded: addresses that are never previewed (the plugin's own: its download link, update pages).
QVector<Link> linksIn(const QTextBlock& block, const QStringList& excluded = {});
QVector<Link> findLinks(QTextDocument* doc, const QStringList& excluded = {}, int maxBlocks = kMaxBlocks);

struct Slot {
    int     separator = -1; // our line break before the picture
    int     object    = -1;
    int     after     = -1; // our line break after it (-1: none)
    QString name;           // the picture's name
    QString urlKey;
    QString href;
    QSizeF  size;
    bool isCard() const { return name.startsWith(cardPrefix()); }
    bool exists() const { return object >= 0; }
};
QVector<Slot> slotsIn(const QTextBlock& block);
QVector<Slot> allSlots(QTextDocument* doc);
// The slot right after link (its line break at link.end, same address), or one that doesn't exist.
Slot slotAfter(const QTextBlock& block, const Link& link);

// ---- edits (the caller keeps the mutating flags set; several may share one edit block) -----------------
// Puts a slot after link: our line break, the picture (name, logical size), and our line break after it when
// the line goes on with text. False when link doesn't describe the document any more or a slot is there.
bool insertSlot(QTextDocument* doc, const Link& link, const QString& name, const QSizeF& size);
// Another picture in the slot whose picture is at object (a card becomes the media preview and back).
bool replaceObject(QTextDocument* doc, int object, const QString& name, const QSizeF& size);
bool resizeObject(QTextDocument* doc, int object, const QSizeF& size);
// The slot whose picture is at object out (its line breaks too); the next line scrubbed.
bool removeSlot(QTextDocument* doc, int object);
// TeamSpeak appends a line with a new block that takes the format of the character before it: after a slot
// at a message's end, our picture's. Block n gets back what it would have had without the slot (the format of
// the character before our line break). False if nothing of ours was there.
bool scrubCopied(QTextDocument* doc, int n);
// Every slot out (names: their pictures' names), every copy scrubbed. Returns how many slots.
int  removeAll(QTextDocument* doc, QStringList* names = nullptr);

} // namespace linkdoc
