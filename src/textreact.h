#pragma once

// 2.3 text reactions: reactions on chat messages that have no media (text, links, replies). Pure QtCore
// (unit-tested): the message identity, the wire format and the index that finds a message again.
//
// TeamSpeak messages have no ids, so every client works one out from what it has in its own chat:
//  * hash: the sender's uid (from the header's client link) and the message's text, normalized so that
//    whatever differs between clients doesn't count: only letters and digits (case-folded) and symbols
//    (emoji) are kept, and TeamSpeak's emoticon codes (":)", ":-D", "8)", ...) are left out, as pictures
//    and as text (icon packs and the "show emoticons" option differ). 16 lower-case hex digits.
//  * time: when this client received it (its header's time and day, as epoch seconds by its own clock).
// The local key of a message is "<hash>-<time>"; reactions.json-like storage keeps reactions by it.
//
// On the wire a message is "<hash>-<age>": how many seconds ago the sender of the plugin command
// received it. Ages are relative, so clock differences and time zones between clients don't matter:
// the receiver looks for a message with that hash which it received about now - age (within
// kToleranceSec, the nearest one). Identical repeated messages are told apart by their time: header
// times are whole seconds on both sides, so an age is off by up to 2 s, and repeats closer than that may
// get each other's reactions (the same text from the same person within one second shares them).
//
// Protocol (tsm1, peerprotocol.h; 2.2 clients ignore both types):
//   TR    s=c|p [y=1] i=<hash>-<age>:<codes>,... [e=<hash>-<age>:<emoji>.<emoji>,...]
//                     the sender's complete reaction set on each message, exactly like R (i= the six v1
//                     codes, e= the whole set when it has other emoji).
//   TSYNC s=c m=<hash>-<age>,...
//                     a late joiner asks present members for their own reactions on these messages; they
//                     answer with TR y=1 (only their own reactions, with their own ages).

#include <QByteArray>
#include <QDate>
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "peerprotocol.h"

namespace textreact {

constexpr int    kHashChars    = 16;
constexpr qint64 kMaxAgeSec    = 31LL * 86400; // older messages get no reactions on the wire
constexpr qint64 kToleranceSec = 6;            // header times are whole seconds; plus the way through TeamSpeak
constexpr qint64 kFutureSec    = 300;          // a message "received" this far ahead of now: its day is wrong
constexpr int    kMaxTextChars = 4000;         // what is read of a message's text (replies::kMaxBodyChars)

// ---- identity ---------------------------------------------------------------------------------------

// TeamSpeak's emoticon codes (every icon pack's emoticons.txt).
const QStringList& emoticonCodes();
// text with the emoticon codes taken out, then only letters, digits and symbols kept (case-folded).
QString normalized(const QString& text);
// The hash of a message by senderUid with this text (normalized here). Empty without a uid.
QString contentHash(const QString& senderUid, const QString& text);
bool    isHash(const QString& hash);

QString localKey(const QString& hash, qint64 timeSec); // empty for an invalid hash or time
bool    parseLocalKey(const QString& key, QString* hash, qint64* timeSec);
bool    isLocalKey(const QString& key);

// "<hash>-<age>" for the wire; empty when age is out of [0, kMaxAgeSec].
QByteArray wireRef(const QString& hash, qint64 ageSec);
bool       parseWireRef(const QByteArray& ref, QString* hash, qint64* ageSec);
// The wire ref of a local key at nowSec (its age clamped at 0 for times up to kFutureSec ahead).
QByteArray wireRefOf(const QString& localKey, qint64 nowSec);

// When a message was received: its day and the header's seconds since midnight, by the local clock
// (offsetSec: a fixed UTC offset instead, for tests). Invalid input: 0.
qint64 epochOf(const QDate& date, int secondsOfDay);
qint64 epochOf(const QDate& date, int secondsOfDay, int utcOffsetSec);

// ---- messages ---------------------------------------------------------------------------------------

// TR: proto::React with wire refs as the items' keys.
std::optional<proto::React> readReact(const proto::Message& message);
// As few TR messages as needed (like proto::makeReacts). Items whose key isn't a wire ref are left out.
QVector<proto::Message> makeReacts(proto::Scope scope, bool syncAnswer, const QVector<proto::ReactItem>& items);
// TSYNC: proto::Sync with wire refs as keys (channel only).
std::optional<proto::Sync> readSync(const proto::Message& message);
proto::Message             makeSync(proto::Scope scope, const QStringList& refs);

// ---- reading chats again ------------------------------------------------------------------------------

// What was read of a chat's blocks, found again after TeamSpeak renumbered them: its line limit drops
// the first blocks with every new line once a chat is full, so every number moves. before / now: each
// block's signature (0: not read); for each block now, the index in before with the same signature
// (the same index first, else the first such one), or -1. moved: whether any reading changed its index.
QVector<int> carryOver(const QVector<quint64>& before, const QVector<quint64>& now, bool* moved = nullptr);
// How far the blocks moved up since that reading (the line limit dropped that many), from carryOver's
// result: the move most readings found again share. 0: nothing moved; -1: nothing was found again (the
// chat was cleared or is another one: what was kept of it doesn't apply).
int movedUp(const QVector<int>& from);
// The days of a chat's blocks now (layoutdoc::datesOf, by block number) with the gaps filled from the days
// an earlier reading used (kept, by its numbering; the blocks moved up by up since): a message above every
// day line left, whose day line went with the dropped lines, keeps its day and so its key. up < 0: now.
QVector<QDate> keepDays(QVector<QDate> now, const QVector<QDate>& kept, int up);

// ---- the index of messages in the chats ---------------------------------------------------------------

enum class ChatKind { Unknown, Server, Channel, Private };
// A chat tab by its place (TeamSpeak's chat tabs: the server's, the channel's, then private chats, and
// the first two can't be closed or moved). Unknown for a negative index.
ChatKind kindOfTab(int index);

struct IndexedMessage {
    QString hash;
    qint64  timeSec = 0;
    QString sender; // uid
};

struct IndexedChat {
    QString                 serverUid;  // empty: not known yet
    ChatKind                kind = ChatKind::Unknown;
    QString                 partnerUid; // a private chat's partner (empty: not known)
    QVector<IndexedMessage> messages;   // document order
};

class Index
{
  public:
    void setChat(quintptr id, const IndexedChat& chat);
    void removeChat(quintptr id);
    void clear();
    int  chatCount() const { return m_chats.size(); }
    int  messageCount() const;

    struct Query {
        QString      serverUid;    // the connection the reaction came on
        proto::Scope scope = proto::Scope::Channel;
        QString      reactorUid;   // who sent it (from TeamSpeak's invoker fields)
        QString      ownUid;       // our identity there (private chats hold only our and the partner's messages)
        QString      hash;
        qint64       estimatedSec = 0; // now - age, by our clock
        qint64       toleranceSec = kToleranceSec;
    };
    // The local key of the message a reaction means: the same hash in a chat that fits the scope (a known
    // server chat never; a channel reaction not in a known private chat; a private one only on a message
    // of the reactor's, outside the channel chat, or on ours in the private chat known to be with the
    // reactor), received nearest to the estimate within the tolerance. Empty: none.
    QString resolve(const Query& query) const;
    // The newest distinct messages of the server's channel chats (local keys), newest first.
    QStringList recentChannelKeys(const QString& serverUid, int max) const;
    bool        knows(const QString& localKey) const;

  private:
    QHash<quintptr, IndexedChat>                  m_chats;
    QHash<QString, QVector<QPair<quintptr, int>>> m_byHash; // hash -> (chat, message index)
};

} // namespace textreact
