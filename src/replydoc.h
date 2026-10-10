#pragma once

// 2.2 reply: reading and changing TeamSpeak's chat document for replies. QtGui only (no widgets), so
// the tests run it on documents built like TeamSpeak's.
//
// S0: a chat message is one QTextBlock, "[icon]<HH:mm:ss> "Nick": text", the nickname linked to
// client://<clid>/<uid>~<nick>; '\n' in a message is U+2028 inside the block. A reply's first line is its
// quote line (replies.h). For plugin users that line is taken out and replaced by one picture of ours
// (the Discord-style reply line) put at the start of the block, followed by a line break of ours: the
// reply line sits above the message's header, and the message keeps its number of lines. The picture's
// format keeps the quote line's text and formats, so restore() gives it back exactly (plugin unload,
// TeamSpeak's own copy of the chat stays as it was received).

#include <QColor>
#include <QSizeF>
#include <QString>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QVector>

#include "replies.h"

class QTextBlock;
class QTextDocument;

namespace replydoc {

// Our reply line picture: an image named objectPrefix() + id, with these properties.
QString        objectPrefix(); // "tsmedia-reply:"
constexpr int  kObjectProperty    = QTextFormat::UserProperty + 0x7460; // 1 on our picture
constexpr int  kRunsProperty      = QTextFormat::UserProperty + 0x7461; // the quote line: text, QTextFormat, text, ...
constexpr int  kOffsetProperty    = QTextFormat::UserProperty + 0x7462; // where it was, from the block start
constexpr int  kSeparatorProperty = QTextFormat::UserProperty + 0x7463; // our line break after the picture
constexpr int  kMaxBlocks         = 4000; // the newest blocks of a chat that are read (older ones: as TeamSpeak shows them)

struct Message {
    int     block    = -1; // block number
    int     position = 0;  // block position
    bool    visible  = true;
    QString nick;
    QString uid;           // from the header's client link (empty without one)
    int     clientId = 0;  // from that link (the id when the message was shown)
    int     minutes  = -1; // the header's time, -1 without one
    QColor  nickColor;     // the nickname's colour in the header (invalid: none set)
    QString text;          // the message's text up to its first file link, as plain text (no quote line)
    QString mediaLabel;    // that link's label, if any
    int     textStart = 0; // document position of the text
    bool    hasQuote  = false; // its first line is a quote line (raw or ours)
    replies::Quote quote;
    bool    restyled = false; // our picture is at the block start
    QString object;           // its name
    QSizeF  objectSize;
    int     quoteStart = -1;  // a raw quote line: [quoteStart, quoteEnd), its line break included
    int     quoteEnd   = -1;
    QTextCharFormat baseFormat; // the header's text format (our picture and line break take its font)
    qreal   lineHeight = 0;     // height of the header's line when it was laid out (0: unknown)
};

// The chat messages among the newest maxBlocks blocks, in document order.
QVector<Message> scan(QTextDocument* doc, int maxBlocks = kMaxBlocks);
// The chat messages from block number firstBlock to the end (after edits that left earlier blocks alone).
QVector<Message> scanFrom(QTextDocument* doc, int firstBlock);
// One block; header-less blocks (TeamSpeak's status lines, plugin prints) give block < 0.
Message parseBlock(const QTextBlock& block);

// The text of an emoticon picture TeamSpeak put in place of ":)" ("emoticons:smile.svg" -> ":)"); empty
// for any other picture.
QString emoticonText(const QString& imageName);

// ---- edits (the caller keeps ChatIntegration's "mutating" flag set) -------------------------------------
// Several edits in a row belong in one edit block of the caller's (QTextCursor::beginEditBlock): the
// chat is then laid out again once instead of once per edit. Layout queries (line heights, sizes) come
// before that block: inside it the layout hasn't seen the edits yet.

// Takes m's raw quote line out and puts our picture (name, logical size) and our line break at the start
// of its block. False if m doesn't describe the document any more.
bool collapse(QTextDocument* doc, const Message& m, const QString& name, const QSizeF& size);
// Gives the quote line back for our picture at position. False if there is none there.
bool restore(QTextDocument* doc, int position);
// Every picture of ours in doc, and line breaks of ours left without one. Returns how many.
int restoreAll(QTextDocument* doc);
// A new size for our picture at position (the chat got wider, another font).
void resizeObject(QTextDocument* doc, int position, const QSizeF& size);
// Positions of our pictures in doc.
QVector<int> objectPositions(QTextDocument* doc);

} // namespace replydoc
