#pragma once

// 2.2 protocol: "tsm1", the plugin-to-plugin protocol carried by TeamSpeak plugin commands
// (sendPluginCommand / onPluginCommandEvent). Pure QtCore: parsing, writing and checking messages, so
// every hostile input can be unit-tested. Nothing here knows about TeamSpeak.
//
// Wire format (ASCII only):
//   command = "tsm1" SP type *(SP field)
//   type    = 1*8 UPPER
//   field   = key "=" value
//   key     = 1*8 lower
//   value   = 0*512 of [A-Za-z0-9._,:-]   (no space, '|', backslash, '=', quotes, controls, non-ASCII)
// Exactly one space between tokens, none at the start or end, at most 32 tokens, no key twice.
// Unknown types and unknown fields are ignored by receivers; "tsm2" and later are ignored too.
//
// Messages (v1):
//   HELLO pv=1 v=2.2.0 caps=p,r rr=1   I'm here (to the channel, or to one private-chat partner).
//   HI    pv=1 v=2.2.0 caps=p,r        the answer, only to whoever asked.
//   BYE                                presence switched off or the plugin unloads.
//   R     s=c|p [y=1] i=<key>:<codes>,... [e=<key>:<emoji>.<emoji>,...]
//                                      my complete reaction set on each media (idempotent).
//   SYNC  s=c m=<key>,...              a late joiner asks present members for their own reactions.
// The sender's identity is never in the payload: receivers take the invoker that the server fills in.
//
// 2.2 emoji: any emoji can be a reaction. i= keeps the six v1 codes (what every 2.2 client reads; an
// item whose set has none of them says "key:" there). e= is added for items whose set has other emoji:
// the whole set in the order it was made, each emoji as its wire code (emojidata.h: "1f923",
// "2764-fe0f"). Receivers that know e= take an item's set from it (unknown emoji are left out) and use
// i= for the rest; older ones ignore the field. A malformed e= is ignored as a whole.

#include <QByteArray>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace proto {

constexpr int kMaxSendBytes    = 900;  // per command we send (TeamSpeak allows 8192, ~9.1 KB escaped)
constexpr int kMaxRecvBytes    = 2048; // longer commands are dropped before they are even copied
constexpr int kMaxTokens       = 32;
constexpr int kMaxValueChars   = 512;
constexpr int kMaxItems        = 20;   // media per R or SYNC
constexpr int kMaxCodesPerItem = 16;   // reaction codes per media (newer versions may know more than 6)

// What onPluginCommandEvent reports as pluginName for our commands: the DLL name without
// "_win64.dll" / "_win32.dll" (S0). ts3plugin_registerPluginID's value is a GUID per load and only
// good for sending.
constexpr const char* kPluginName = "tsmedia";

// ---- reactions ------------------------------------------------------------------------------------

// The six reactions of v1, in their fixed display order. A set of them is a 6-bit mask.
constexpr int    kReactionCount = 6;
constexpr quint8 kAllReactions  = 0x3f;
enum Reaction { ThumbsUp = 0, Heart, Laughing, Surprised, Sad, Fire };

// Wire code of a reaction: "up", "heart", "lol", "wow", "sad", "fire"; nullptr out of range.
const char* reactionCode(int index);
// The reaction with this wire code, -1 for an unknown (newer) one.
int reactionIndex(const QByteArray& code);
// "up.heart" in display order; empty for an empty mask.
QByteArray maskToCodes(quint8 mask);
// The known reactions of a '.'-separated code list (unknown codes are left out). False when the list
// itself is malformed (bad characters, empty codes, too many); "" is a valid empty set.
bool codesToMask(const QByteArray& codes, quint8* mask);

// A media id: MediaLink::key(), 20 lower-case hex digits.
bool isMediaKey(const QByteArray& key);
bool isMediaKey(const QString& key);

// ---- 2.2 emoji: reactions with any emoji ---------------------------------------------------------------

// A reactor's complete set on one media: emoji ids (emojidata.h) in the order they were added, distinct,
// at most kMaxReactionsPerSet and kMaxSetCodeChars of wire codes.
using ReactionSet = QVector<int>;
constexpr int kMaxReactionsPerSet  = 10;
constexpr int kMaxSetCodeChars     = 400; // e= text of one item, so every item fits into one message
constexpr int kMaxDistinctReactions = 20; // different reactions shown on one media

// The emoji of a v1 reaction (👍 ❤️ 😂 😮 😢 🔥); -1 out of range. And back: the v1 index of an emoji, -1.
int legacyReactionEmoji(int index);
int legacyReactionIndex(int emojiId);

quint8      legacyMask(const ReactionSet& set); // the v1 reactions in set
ReactionSet setFromMask(quint8 mask);           // in v1 display order
// Valid ids, each once, within the limits (later ones are left out).
ReactionSet cleanSet(const ReactionSet& set);
// The same reactions in the same order. (Not QVector's ==: with Qt 5.15 and a current MSVC it uses a
// deprecated checked iterator, which /W4 reports.)
bool sameSet(const ReactionSet& a, const ReactionSet& b);
// Whether one more emoji still fits into set (kMaxReactionsPerSet, kMaxSetCodeChars).
bool canAdd(const ReactionSet& set, int emojiId);
// "1f44d.1f923" for e=; and back: false for a malformed list (characters, empty or malformed codes, too
// many); unknown emoji are left out, duplicates once.
QByteArray setToEmojiCodes(const ReactionSet& set);
bool       emojiCodesToSet(const QByteArray& codes, ReactionSet* set);

// ---- messages -------------------------------------------------------------------------------------

struct Message {
    QByteArray                             type; // "HELLO", "R", ...
    QVector<QPair<QByteArray, QByteArray>> fields;

    bool       has(const char* key) const;
    QByteArray value(const char* key) const; // empty if missing
    void       set(const char* key, const QByteArray& value);
};

enum class ParseResult {
    Ok,
    Empty,
    TooLong,      // over kMaxRecvBytes
    NotOurs,      // another magic
    NewerVersion, // "tsm2" and later
    Malformed,    // breaks the grammar (characters, spacing, tokens, duplicate keys, ...)
};

// Cheap check on TeamSpeak's callback thread, before anything is copied: at most kMaxRecvBytes and
// starts with our magic.
bool looksLikeOurs(const char* raw);

std::optional<Message> parse(const QByteArray& raw, ParseResult* result = nullptr);
// The wire form; empty if the message breaks the grammar or would be over kMaxSendBytes.
QByteArray serialize(const Message& message);

// ---- typed messages -------------------------------------------------------------------------------

// HELLO and HI.
struct Hello {
    bool        hi = false;     // a HI (an answer)
    QString     version;        // the sender's TS Media version, display only; empty if malformed
    QStringList caps;           // what it supports: "p" presence, "r" reactions; unknown ones too
    bool        replyRequested = false; // rr=1

    bool hasCap(const char* cap) const;
};
// HELLO / HI whose pv lists protocol 1; nullopt otherwise (wrong type, no common version).
std::optional<Hello> readHello(const Message& message);
Message              makeHello(const QString& version, const QStringList& caps, bool replyRequested);
Message              makeHi(const QString& version, const QStringList& caps);
Message              makeBye();

enum class Scope { Channel, Private }; // s=c, s=p (the server chat is not allowed)

struct ReactItem {
    QString     key;      // media key
    quint8      mask = 0; // the v1 part of the sender's set (what 2.2.0 receivers see); 0 = none of them
    ReactionSet set;      // 2.2 emoji: the whole set; empty with a mask: the mask's reactions
    ReactionSet reactions() const { return set.isEmpty() ? setFromMask(mask) : set; }
};

struct React {
    Scope              scope      = Scope::Channel;
    bool               syncAnswer = false; // y=1
    QVector<ReactItem> items;              // 1..kMaxItems, each key once
};
std::optional<React> readReact(const Message& message);
// As few R messages as needed for items: each within kMaxSendBytes, kMaxValueChars and kMaxItems.
// Items with an invalid key are left out.
QVector<Message> makeReacts(Scope scope, bool syncAnswer, const QVector<ReactItem>& items);

struct Sync {
    Scope       scope = Scope::Channel;
    QStringList keys; // 1..kMaxItems distinct media keys
};
std::optional<Sync> readSync(const Message& message);
// The first kMaxItems distinct valid keys (an empty message if there are none).
Message makeSync(Scope scope, const QStringList& keys);

} // namespace proto

Q_DECLARE_METATYPE(proto::Message)
