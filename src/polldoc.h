#pragma once

// 2.3 poll: poll messages in TeamSpeak's chat document. QtGui only (no widgets): the tests run it on
// documents built like TeamSpeak's (tests/tschat.h).
//
// A poll message (polls.h) is one block: TeamSpeak's header, then the question line, the option lines and
// the marker link "Vote with TS Media chat". For plugin users that text (from the body's start to the end of
// the marker link) is taken out and replaced by one picture, the poll card, with our line break (U+2028 in a
// 1 px font) in front where the card would otherwise share the header's line (normalize()).
// The picture's format keeps what was taken out (HD emoji as their text, TeamSpeak's emoticons as its
// pictures), so restore() gives it back exactly (plugin unload), and the poll as read, for drawing.
// Everything read from a chat is untrusted: only plain strings and checked numbers come out of it.

#include <QSizeF>
#include <QString>
#include <QTextFormat>
#include <QVector>

#include "polls.h"

class QTextBlock;
class QTextCharFormat;
class QTextDocument;

namespace polldoc {

// Our picture: an image named objectPrefix() + n, with these properties (other modules use 0x7453-0x7489).
QString       objectPrefix(); // "tsmedia-poll:"
constexpr int kObjectProperty    = QTextFormat::UserProperty + 0x74b0; // QString: the poll's key
constexpr int kRunsProperty      = QTextFormat::UserProperty + 0x74b1; // what was taken out: text, QTextFormat, ...
constexpr int kOffsetProperty    = QTextFormat::UserProperty + 0x74b2; // where it was, from the block start
constexpr int kPollProperty      = QTextFormat::UserProperty + 0x74b3; // QStringList: the poll as read
constexpr int kSeparatorProperty = QTextFormat::UserProperty + 0x74b4; // our line break before the picture
constexpr int kMaxBlocks         = 4000; // the newest blocks of a chat that are read

struct Found {
    int         block         = -1; // block number
    int         blockPosition = 0;
    int         from          = 0;  // the poll's text with its marker link: [from, to)
    int         to            = 0;
    polls::Poll poll;
    bool        collapsed      = false; // our picture is there (objectPosition)
    int         objectPosition = -1;
    QString     object;
    QSizeF      size;
};

// The poll messages among the newest maxBlocks blocks (collapsed or not), in document order.
QVector<Found> scan(QTextDocument* doc, int maxBlocks = kMaxBlocks);
Found          parseBlock(const QTextBlock& block); // block < 0: no poll there

// ---- edits (one edit block each) --------------------------------------------------------------------------
// Takes found's text out and puts our line break and picture (name, logical size) there. False if found
// doesn't describe the document any more.
bool         collapse(QTextDocument* doc, const Found& found, const QString& name, const QSizeF& size);
bool         restore(QTextDocument* doc, int position); // our picture at position
// Our line break in front of the card only where the block's look needs it (TeamSpeak's header and a
// Compact head: yes; a Cozy head's body and a continuation: no). After TS Media's layout styled a block, or
// gave it back. True / the count: what changed.
bool         normalize(QTextDocument* doc, int position);
int          normalizeAll(QTextDocument* doc);
int          restoreAll(QTextDocument* doc);
int          giveBack(QTextDocument* doc); // restoreAll, and the pictures' memory goes too
void         resizeObject(QTextDocument* doc, int position, const QSizeF& size);
QVector<int> objectPositions(QTextDocument* doc);
bool         isObject(const QTextCharFormat& format);

QStringList pollToList(const polls::Poll& poll);
polls::Poll pollFromList(const QStringList& list); // invalid when the list is malformed

// What our picture reads as. summaryText: "📊 Poll: <question>" (a reply's quote, the chat's searches);
// copyText: the message's text as TeamSpeak showed it (copying from the chat). Empty for other formats.
QString summaryText(const QTextCharFormat& format);
QString copyText(const QTextCharFormat& format);

} // namespace polldoc
