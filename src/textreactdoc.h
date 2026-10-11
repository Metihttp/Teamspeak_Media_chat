#pragma once

// 2.3 text reactions in TeamSpeak's chat document. QtGui only (the tests and the harness run it on
// documents built like TeamSpeak's, tests/tschat.h).
//
// Which blocks: chat messages (layoutdoc::RowType::Message, read through every layer of ours) that have
// no media: no ts3file link and no preview, card or album of ChatIntegration's. Their identity is
// textreact's hash of the header's uid and the message's text (replydoc::parseBlock: after a reply's quote
// line, HD emoji and TeamSpeak's emoticons as their text) and the time the header shows.
//
// The reaction row: at the end of the message's block, a line break of ours (U+2028) and one picture
// "tsmedia-treact:<n>" (the pills; drawn by ChatTextReactions). Both carry properties of ours
// (0x74c0-0x74c2; the previews and albums use 0x7453-0x7456, the HD emoji 0x7460-0x7463, the replies
// 0x7470-0x7473, the layout 0x7480-0x748a), so they are found by property, never by a remembered
// position, and every other module skips them. They sit inside the block: the layout's hanging indent
// puts the row under the message's text, a bubble around the message holds it too. Blocks are never
// added or removed. Everything read from the chat is untrusted: nothing in it is run or fetched.

#include <QDate>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QVector>

class QTextBlock;
class QTextDocument;

namespace textreactdoc {

constexpr int kRowObject    = QTextFormat::UserProperty + 0x74c0; // bool: our row picture
constexpr int kRowKey       = QTextFormat::UserProperty + 0x74c1; // QString: the message's local key
constexpr int kRowSeparator = QTextFormat::UserProperty + 0x74c2; // bool: our line break before it

QString objectPrefix(); // "tsmedia-treact:"

inline bool isRowObject(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.boolProperty(kRowObject) && format.toImageFormat().name().startsWith(QLatin1String("tsmedia-treact:"));
}

inline bool isRowSeparator(const QTextCharFormat& format)
{
    return format.boolProperty(kRowSeparator);
}

// ---- reading ------------------------------------------------------------------------------------------

struct Identity {
    bool    message = false; // a chat message (any)
    bool    media   = false; // ... with a file link or a preview: its reactions are the media's
    QString uid;             // the header's client link
    QString text;            // its text (no quote line; emoji and emoticons as text)
    int     seconds = -1;    // the header's time of day; -1 without timestamps
    QString hash;            // textreact::contentHash(uid, text); empty without a uid
    bool    eligible() const { return message && !media && !hash.isEmpty() && seconds >= 0; }
};
Identity identify(const QTextBlock& block);

// The day a message was received: date (from the chat's day lines and history markers, layoutdoc's
// datesOf) or, without one, today, or yesterday for a time more than textreact::kFutureSec after now.
QDate dayOf(const QDate& date, int seconds, const QDate& today, int nowSeconds);
// The local key of an eligible message received on day (textreact::localKey of its hash and time; the
// local clock's time zone, or a fixed UTC offset for tests). Empty when it isn't eligible.
QString localKeyOf(const Identity& id, const QDate& day, qint64* timeSec = nullptr);
QString localKeyOf(const Identity& id, const QDate& day, int utcOffsetSec, qint64* timeSec = nullptr);

// ---- the row --------------------------------------------------------------------------------------------

struct Row {
    int              separator = -1; // positions; -1: none
    int              object    = -1;
    QTextImageFormat format;
    QString          key;
    bool exists() const { return object >= 0; }
};
Row rowOf(const QTextBlock& block);
// Where block's row is drawn (document coordinates, the document laid out); empty without one. A bubble
// around the message (chat visuals) holds this rect too.
QRectF rowRect(const QTextBlock& block);

// Edits (the caller keeps layoutformat's mutating flag set; several may share one edit block).
// Puts a row (picture name, logical size, the message's key) at the end of block n. False if one is there.
bool insertRow(QTextDocument* doc, int n, const QString& name, const QString& key, const QSizeF& size);
// A new size for block n's row. False without a row.
bool resizeRow(QTextDocument* doc, int n, const QSizeF& size);
// Takes block n's row out (and a line break of ours left without one), and scrubs block n + 1. False if
// nothing was there.
bool removeRow(QTextDocument* doc, int n);
// TeamSpeak appends a line with a new block at the end (QTextCursor::insertBlock): the block takes the
// format of the character before it as its own, after a row our picture's (and so would text put into it
// without a format). Block n gets back what TeamSpeak copied without the row: the format of the message's
// last character before our line break. False if nothing of a row's was there.
bool scrubCopied(QTextDocument* doc, int n);
// Every row of ours out of doc, and every copy of one scrubbed; their picture names go to names (may be
// null). Returns how many rows.
int removeAll(QTextDocument* doc, QStringList* names = nullptr);

} // namespace textreactdoc
