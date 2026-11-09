#pragma once

// 2.4 "Load link previews automatically": the rules of the automatic loads, pure (QtCore + Settings) and
// unit-tested; ChatLinkPreviews feeds them and starts the loads.
//
//  * The waits: a slot of our own message waits for our own sender pipeline ("Making the preview…", also
//    with the switch off: it never contacts anything); a slot of someone else's message waits for the
//    sender's preview while the sender makes them (their HELLO/HI capability "l", or we are still asking)
//    and is still here, until the link's wait runs out (waitSec per kind, after the sender's links before it:
//    SenderQueue) or their plugin says it won't make one (LPNO).
//  * The caps: an automatic website load brings at most autoCap() (the automatic download limits of
//    uploaded media; YouTube's title and thumbnail always; with data saver nothing else), a click
//    clickCap().
//  * The budget: kMaxRunning automatic loads at once (all chats; kMaxPerSender of them for one sender's
//    links), and per sender a token bucket (SenderBudget: a few at once, then one every few seconds), so
//    one person can't make everyone fetch hundreds of addresses or hold the automatic loads.

#include <QHash>
#include <QString>

#include <functional>

#include "linkpreview/linkurl.h"

struct Settings;

namespace linkpreview {
namespace autoload {

constexpr int kSenderQueueSec = 120; // SenderPipeline::Limits::queuedTtlMs: a job that couldn't start by then is dropped (LPNO)
constexpr int kOwnGraceSec    = 8;   // our own fresh message waits this long for our pipeline to see it
constexpr int kMaxRunning     = 2;   // automatic website loads at once (all chats) ...
constexpr int kMaxPerSender   = 1;   // ... and of one sender's links (a slow website of theirs never holds both)

enum class Chat { Unknown, Server, Channel, Private };
enum class SenderCap { Unknown, Checking, Yes, No }; // peers::PeerDirectory::CapState
enum class OwnRecord { None, Pending, Done };        // Pending: queued or fetching; Done: any other state
// Sender: the sender makes previews (cap "l"); Checking: we are still asking whether they do (no claim either way).
enum class Wait { None, Own, Sender, Checking };

struct Input {
    bool      autoOn    = true;  // Settings::autoLoadLinkPreviews
    bool      makeOn    = true;  // Settings::showLinkPreviews && Settings::makeLinkPreviews
    Chat      chat      = Chat::Unknown;
    bool      own       = false; // our own message
    OwnRecord ownRecord = OwnRecord::None;
    qint64    timeSec   = -1;    // the message's time (its key); -1: no key
    qint64    nowSec    = 0;
    int       budgetSec = 0;     // how long after timeSec this link waits (SenderQueue::add)
    SenderCap cap       = SenderCap::Unknown;
    bool      senderHere = false; // the sender is connected (and, in a channel chat, in our channel)
    bool      skipped    = false; // the sender's plugin said it won't make this preview (LPNO)
};

// What the slot waits for (None: the normal rules: an automatic load when it may, else the click card).
Wait waitFor(const Input& in);

// The sender of a message as the waits read it: one of their clients (0: not on the server now), and whether a client
// of theirs is in our channel (connected twice with one identity: either one counts).
struct Presence {
    quint16 client       = 0;
    bool    inOurChannel = false;
};
// Input::senderHere: a private chat's partner is connected; a channel chat's sender has a client in our channel.
bool senderHere(Chat chat, const Presence& presence);

// Presence lookups (each walks every client of the server), kept kTtlMs per (connection, uid). One answer serves every
// chat kind: what a private chat's lookup found is just as right for the channel chat.
class PresenceMemo
{
  public:
    static constexpr qint64 kTtlMs      = 1000;
    static constexpr int    kMaxEntries = 512;
    using Lookup                         = std::function<Presence(quint64 sch, const QString& uid)>;

    Presence get(quint64 sch, const QString& uid, qint64 nowMs, const Lookup& lookup);
    void     clear() { m_entries.clear(); }
    int      size() const { return m_entries.size(); }

  private:
    struct Entry {
        Presence presence;
        qint64   atMs = 0;
    };
    QHash<QString, Entry> m_entries;
};

// How long a link of this kind waits for its sender's preview once their pipeline gets to it: YouTube 30 s,
// pictures and GIFs 45 s, videos 120 s (the TeaSpeak video took 67 s: fetch, upload and the poster's content check).
int waitSec(Kind kind);
// The longest a link of this kind waits from its message's time: the sender's queue (kSenderQueueSec), then its own
// waitSec (YouTube 150 s, pictures and GIFs 165 s, videos 240 s).
int maxWaitSec(Kind kind);

// The sender's pipeline makes one preview at a time across all its messages, in the order it sent them, and drops a
// job that couldn't start within kSenderQueueSec (it sends LPNO for it then). SenderQueue follows that over a chat's
// links in the chat's order (time order), per sender: a link waits for the sender's links before it, as far as they
// can still hold it up, and then for its own waitSec. A picture posted 10 s after a slow video waits for the video's
// turn too, not only its own 45 s.
class SenderQueue
{
  public:
    // The wait of this link of sender's message at timeSec, counted from timeSec: from waitSec(kind) up to
    // maxWaitSec(kind). 0 (and nothing counted) for a message without a key (timeSec < 0) or Kind::None.
    int  add(const QString& sender, qint64 timeSec, Kind kind);
    void clear() { m_busyUntil.clear(); }

  private:
    QHash<QString, qint64> m_busyUntil; // sender -> when their pipeline is through their links so far (seconds)
};

// The most a click loads: YouTube 2 MB (its thumbnail), pictures and GIFs 25 MB, videos 50 MB.
qint64 clickCap(Kind kind);
// The most an AUTOMATIC website load may bring, 0: never automatic. YouTube always clickCap (a preview, small;
// also with data saver); pictures and GIFs Settings::autoDownloadMaxMB if autoDownloadImages; videos
// Settings::videoAutoDownloadMB, or when that is 0 (the default, "when you press play") the picture limit
// (a website's video has no poster without its file); never above clickCap; 0 with data saver (the server's
// own, serverUid empty: the setting for all servers).
qint64 autoCap(Kind kind, const Settings& s, const QString& serverUid);

// Per sender ("<server uid>/<sender uid>"): how many automatic website loads their links may start, a token
// bucket (burst at once, then one per refillMs). Bounded: at most maxSenders, full buckets forgotten first.
class SenderBudget
{
  public:
    struct Limits {
        int    burst      = 6;
        qint64 refillMs   = 10000;
        int    maxSenders = 256;
    };
    SenderBudget();
    explicit SenderBudget(const Limits& limits);

    bool has(const QString& sender, qint64 nowMs) const; // a token is there (nothing taken)
    bool take(const QString& sender, qint64 nowMs);      // takes one; false: none
    void clear();
    int  size() const { return m_buckets.size(); }

  private:
    struct Bucket {
        double tokens = 0.0;
        qint64 atMs   = -1;
    };
    double tokensAt(const Bucket& b, qint64 nowMs) const;

    Limits                 m_limits;
    QHash<QString, Bucket> m_buckets;
};

} // namespace autoload
} // namespace linkpreview
