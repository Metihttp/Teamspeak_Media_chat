#pragma once

// 2.3 chat-visuals: people's avatars for the chat's heads (Cozy). Where a picture is looked for, in order:
//  1. TeamSpeak's own file for the client, while it is on the server: ts3Functions.getAvatar (your own avatar
//     too, which TeamSpeak may keep nowhere else), and the path ts3plugin_onAvatarUpdated gave last;
//  2. TeamSpeak's cache file of the uid on this server (people who left: the cache is by uid);
//  3. the same uid's cache file on any other server TeamSpeak has a cache of, newest first (one identity, one
//     person);
// initials otherwise, and always when TeamSpeak says someone on the server has no avatar (no old picture of
// theirs from a cache). TS Media downloads nothing: these are TeamSpeak's files (getAvatar may make TeamSpeak
// fetch a missing one from the server it is connected to, as it does when you click someone). They are
// untrusted pictures: bounded in size, decoded by QImageReader only, on a worker.

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>

namespace chatavatars {

constexpr int    kPixels    = 48;              // decoded at this size (24 px at 2x)
constexpr qint64 kMaxBytes  = 1024 * 1024;
constexpr int    kMaxSide   = 4096;
constexpr int    kMaxServers = 256;            // cache folders looked at for another server's copy

// TeamSpeak's cache name of a uid's avatar: "avatar_" and the uid's bytes in hex with 0-f written as a-p
// (empty for anything that isn't a base64 uid: no way out of the cache folder).
QString cacheFileName(const QString& uid);
// <configDir>/cache/<base64 of the server uid>/clients/<cacheFileName(uid)> (empty if either is unknown).
QString cachePath(const QString& configDir, const QString& serverUid, const QString& uid);

struct Query {
    QStringList paths;     // tried in order
    QString     configDir; // then <configDir>/cache/*/clients/<fileName>, newest first
    QString     fileName;

    bool isEmpty() const { return paths.isEmpty() && fileName.isEmpty(); }
};
struct Found {
    QImage    image; // null: none found
    QString   file;
    QDateTime modified;
    qint64    size = 0;
};
// On a worker: the first file that decodes.
Found find(const Query& query);
// One file: its first frame cropped to a centred square, kPixels big (null if it isn't a picture we take).
QImage decode(const QString& path);
// Whether a found file is still the same (a stat; on the GUI thread now and then).
bool unchanged(const Found& found);

// TeamSpeak's own avatar file of a client on sch (ts3Functions.getAvatar; GUI thread). Empty if it has none,
// or none yet (TeamSpeak then fetches it from its server and calls ts3plugin_onAvatarUpdated). none:
// set when TeamSpeak says the client has no avatar at all (ERROR_database_empty_result).
QString fromTeamSpeak(quint64 sch, quint16 clientId, bool* none = nullptr);

// Where uid's avatar on serverUid is looked for, in order (find): TeamSpeak's file (teamSpeak), the one
// onAvatarUpdated gave (remembered), this server's cache, then the other servers' caches. An empty query
// (initials) for what isn't a uid, and when TeamSpeak says the person on the server has no avatar (none):
// never an old picture of theirs from a cache.
Query queryFor(const QString& teamSpeak, bool none, const QString& remembered, const QString& configDir, const QString& serverUid, const QString& uid);

} // namespace chatavatars
