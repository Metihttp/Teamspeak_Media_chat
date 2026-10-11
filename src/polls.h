#pragma once

// 2.3 poll: anonymous polls like Telegram's. Pure logic (QtCore): the poll message, the ballots and their
// hash chains, the tally, the plugin-command messages and the local store (polls.json). The chat side is
// chatpolls.* (the card, the window, the menu entry) and pollhub.* (sending and receiving).
//
// The poll message is an ordinary chat message that people without the plugin read as text:
//
//   📊 Poll: <question>
//   1. <option>
//   ...
//   [URL=<download page>#tsmpoll1-<key>-<n><s|m>-<channel>]Vote with TS Media chat[/URL]
//
// key: 16 lower-case hex digits (random, never all zero), n: 2..10 options, s/m: one answer or multiple
// answers, channel: the channel it was posted in (votes are only taken there). Plugin users get a card
// instead (question, "Anonymous poll", options, results after voting). Text is bbcodeLiteral()'d.
//
// Ballots (ANONYMOUS): a vote is a ballot, never a person. A voter's client makes a random secret s per
// poll and the chain h(0) = s, h(i+1) = H(key, h(i)) (H: the first 16 bytes of SHA-256); the ballot's id
// is h(kChainLength). Each state the voter publishes (a vote, a change, a retraction: options 0) carries a
// step j (1, 2, ...) and the proof h(kChainLength - j). Anyone can check a state against the ballot's id
// or a state seen before (hashing the proof forward), but only the secret's owner can make a later one:
// the newest step wins everywhere, nobody can take a ballot further (change it, take it back) but its voter,
// and nothing in a ballot names its voter. Everyone stores the ballots they have seen and passes them on to
// late joiners (PQ / PV y=1), so the count survives people leaving; the secret stays on the voter's own disk.
// Only ballots of polls the chat shows are taken (PollHub::watch).
//
// Honest limits (docs/POLLS.md): the server tells every receiver who sent each plugin command, so a
// modified client that logs plugin commands can link a live vote to the person who cast it (TS Media
// never stores or shows that), and an answer for a late joiner to its sender when nobody else had that
// ballot. A modified client can also invent ballots, and (the proof doesn't cover the options) pass on a
// ballot's newest state with other options to people who hadn't got that state yet, until its voter changes
// it again: counts are as trustworthy as the people in the channel. A poll's key is in its message, so a copy
// of the message shows the same counts.
//
// Wire (tsm1, peerprotocol.h):
//   PV q=<key> v=<entry>,<entry>... [y=1]   ballot states (y=1: passed on for a late joiner)
//   PQ q=<key>:<count>:<digest>,...          "what do you have?" with what the asker has
//   entry = <id>.<step>.<proof>.<options>    id, proof: 22 base64url characters (16 bytes); step 1..kChainLength;
//                                            options: lower-case hex bit mask (bit i = option i + 1), 0 = none

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "peerprotocol.h"

namespace polls {

constexpr int kMaxQuestionChars = 200; // UTF-16 units, as typed
constexpr int kMaxOptionChars   = 100;
constexpr int kMinOptions       = 2;
constexpr int kMaxOptions       = 10;
constexpr int kChainLength      = 120;  // states per ballot (votes, changes, retries)
constexpr int kMaxBallots       = 1000; // per poll; more are ignored
constexpr int kMaxPolls         = 300;  // in the store; the least recently active go first (your own votes last)
constexpr int kMaxStoredBallots = 60000; // in the store (polls.json stays far below its 8 MB load limit)
constexpr int kMaxEntries       = 8;    // ballot states per PV
constexpr int kMaxQueried       = 8;    // polls per PQ
constexpr int kKeyChars         = 16;

QString markerPrefix(); // "tsmpoll1-"

struct Poll {
    QString     key;
    QString     question;
    QStringList options;
    bool        multiple  = false;
    quint64     channelId = 0;

    bool isValid() const;
};

// A random key.
QString newKey();
bool    isKey(const QString& key);
bool    isKey(const QByteArray& key);

// One line as typed, made safe and short: controls and bidi marks out, whitespace runs one space, trimmed,
// at most max UTF-16 units (never half an emoji).
QString cleanLine(const QString& typed, int max);

// ---- the message ------------------------------------------------------------------------------------

QString markerFragment(const Poll& poll); // "tsmpoll1-3f9a1c2e5b6d7e80-3s-12"
// The chat message (BBCode). pageUrl: an http(s) page for people without the plugin (its fragment is
// replaced by the marker).
QString composeMessage(const Poll& poll, const QString& pageUrl);

struct Marker {
    QString key;
    int     count     = 0;
    bool    multiple  = false;
    quint64 channelId = 0;
    bool    valid() const { return !key.isEmpty(); }
};
// The marker in a link's href (http(s) with a "#tsmpoll1-..." fragment); invalid otherwise.
Marker parseMarker(const QString& href);

// The poll from the message's text as the chat shows it (everything before the marker link: the
// question line and the option lines, separated by U+2028 or '\n'), checked against the marker. Nullopt
// when it doesn't read as the poll the marker announces.
std::optional<Poll> parseText(const QString& text, const Marker& marker);

// ---- ballots --------------------------------------------------------------------------------------------

struct Ballot {
    QByteArray id;    // 16 bytes
    int        step = 0;
    QByteArray proof; // 16 bytes
    quint16    mask = 0;
};

QByteArray chainNext(const QString& key, const QByteArray& value);
QByteArray chainAt(const QString& key, const QByteArray& secret, int index);
QByteArray newSecret();
// The state of the ballot of secret at step (1..kChainLength) with mask.
Ballot     makeBallot(const QString& key, const QByteArray& secret, int step, quint16 mask);
// Whether update may follow what is known: known.step 0 means only the id is known (known.id).
bool       follows(const QString& key, const Ballot& known, const Ballot& update);

QByteArray            entryText(const Ballot& ballot);
std::optional<Ballot> parseEntry(const QByteArray& text);

// A mask that fits a poll: options only within count, exactly one for a one-answer poll.
bool validMask(quint16 mask, int count, bool multiple);

struct Tally {
    QVector<int> counts; // per option
    int          voters = 0;
    int          percent(int option) const; // of the voters, rounded; 0 without voters
};
Tally tally(const QVector<Ballot>& ballots, int count, bool multiple);
// Percentages of a one-answer poll that add up to 100 (largest remainder), else percent() each.
QVector<int> percentages(const Tally& tally, bool multiple);

// ---- messages ---------------------------------------------------------------------------------------------

struct Votes {
    QString         key;
    QVector<Ballot> ballots;
    bool            answer = false; // y=1
};
// As few PV messages as needed (kMaxEntries each).
QVector<proto::Message> makeVotes(const QString& key, const QVector<Ballot>& ballots, bool answer);
std::optional<Votes>    readVotes(const proto::Message& message);

struct Digest {
    QString    key;
    int        count = 0;
    QByteArray hash; // 8 base64url characters
    bool operator==(const Digest& o) const { return key == o.key && count == o.count && hash == o.hash; }
};
proto::Message                 makeQuery(const QVector<Digest>& digests); // the first kMaxQueried
std::optional<QVector<Digest>> readQuery(const proto::Message& message);

// ---- the store ---------------------------------------------------------------------------------------------

class Store
{
  public:
    // The own ballot on a poll: kept on this computer only.
    struct Own {
        QByteArray secret;
        QByteArray id;            // the ballot's id (from the secret; not saved)
        int        step      = 0; // the last step used
        quint16    mask      = 0; // what the others should have (0: no vote)
        quint16    confirmed = 0; // the last mask the server took
        int        confirmedStep = 0;
    };

    enum class Merge { Ignored, Added, Updated };

    // now: wall-clock ms (the least recently active polls go when there are too many). A newer state of your
    // own ballot than you remember (polls.json wasn't written after a vote) becomes your vote again, so the
    // next one goes on from the step the others have.
    Merge   merge(const QString& key, const Ballot& ballot, qint64 now);
    // Your vote: the next state of your ballot (a new secret on the first vote). Nullopt when the chain is
    // used up (kChainLength states).
    std::optional<Ballot> castOwn(const QString& key, quint16 mask, qint64 now);
    void    confirmOwn(const QString& key, int step, quint16 mask);

    bool             has(const QString& key) const { return m_polls.contains(key); }
    QVector<Ballot>  ballots(const QString& key) const;
    Ballot           ballot(const QString& key, const QByteArray& id) const; // step 0 if unknown
    Digest           digest(const QString& key) const;
    Tally            tally(const QString& key, int count, bool multiple) const;
    std::optional<Own> own(const QString& key) const;
    int              pollCount() const { return m_polls.size(); }
    int              ballotCount() const { return m_ballotCount; } // in all polls

    QJsonObject toJson() const;
    bool        fromJson(const QJsonObject& json); // false: not ours / malformed (nothing kept)
    bool        load(const QString& path);
    bool        save(const QString& path) const;

  private:
    struct Entry {
        QHash<QByteArray, Ballot> ballots; // by id
        qint64                    touched = 0;
        std::optional<Own>        own;
    };
    Entry& entry(const QString& key, qint64 now);
    // The least recently active polls go (never keep; polls with your vote only when nothing else is left)
    // until at most kMaxPolls polls and kMaxStoredBallots ballots are kept.
    void   trim(const QString& keep);
    void   recount();

    QHash<QString, Entry> m_polls;
    int                   m_ballotCount = 0;
};

} // namespace polls
