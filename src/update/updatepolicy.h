#pragma once

// The updater's rules that don't touch the network or the disk: when to check, where requests may
// go, what the restarted TeamSpeak gets on its command line. Pure QtCore, unit-tested.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace upd {

// ---- addresses ---------------------------------------------------------------------------------

// Not user-configurable. The plugin builds every URL itself from the validated version.
constexpr const char* kRepository = "Metihttp/Teamspeak_Media_chat";

// The release's manifests (updatemanifest.h): the format 2 file, read first, and the format 1 file that
// 2.2.0 reads, read only when the latest release has no format 2 file (404).
constexpr const char* kManifestName       = "tsmedia-update-v2.json";
constexpr const char* kLegacyManifestName = "tsmedia-update.json";

QUrl manifestUrl();                          // .../releases/latest/download/tsmedia-update-v2.json
QUrl legacyManifestUrl();                    // .../releases/latest/download/tsmedia-update.json
QUrl assetUrl(const QString& tag, const QString& asset); // .../releases/download/<tag>/<asset>
QUrl releaseNotesUrl(const QString& tag);    // .../releases/tag/<tag>
QUrl latestReleaseUrl();                     // .../releases/latest (the manual download page)

// Requests and every redirect hop must go to https://github.com or https://<sub>.githubusercontent.com
// on port 443, without user info. Only TSMEDIA_TESTHOOKS builds can add one local test origin.
bool isAllowedUrl(const QUrl& url);
constexpr int kMaxRedirects = 5;

#ifdef TSMEDIA_TESTHOOKS
// "http://127.0.0.1:8123": manifest and asset URLs then point there (same paths as on GitHub), and
// only that origin is allowed. Refused (returns false) for anything but a loopback host.
bool setTestOrigin(const QString& origin);
QString testOrigin();
#endif

// ---- schedule ----------------------------------------------------------------------------------

// rand is any 32-bit random number; the functions map it into their range so tests can pin it.
qint64    initialDelayMs(quint32 rand);                         // 3..8 min after the plugin starts
QDateTime nextAfterSuccess(const QDateTime& now, quint32 rand); // now + 24 h + 0..3 h
// failures counts this failure too (1, 2, ...): 1 h, 3 h, 6 h, 12 h, then 24 h. retryAfterSec (from a
// 403/429 answer, already clamped) can only push it later.
QDateTime nextAfterFailure(const QDateTime& now, int failures, qint64 retryAfterSec = 0);
// A next-check time more than 48 h ahead (clock changed, edited file) becomes now + 24 h.
QDateTime sanitizeNext(const QDateTime& next, const QDateTime& now);
constexpr qint64 kRemindAfterSec    = 20 * 3600; // "Later" asks again after this
constexpr int    kMaxConsentAsks    = 2;
constexpr int    kConsentDelayMs    = 45 * 1000;
constexpr int    kRecheckIntervalMs = 30 * 60 * 1000;
constexpr qint64 kMaxRetryAfterSec  = 24 * 3600;

// updateCheck in settings.ini: 0 = not asked yet (no requests), 1 = on, 2 = off.
enum class CheckSetting { NotAsked = 0, On = 1, Off = 2 };
bool shouldAskConsent(CheckSetting setting, int timesAsked);

// ---- download deadlines ------------------------------------------------------------------------

constexpr qint64 kCheckDeadlineMs = 60 * 1000;
// max(120 s, size at 16 KB/s): a server that drips data can't keep the worker (and the DLL) alive.
qint64 downloadDeadlineMs(qint64 bytes);

// ---- restart -----------------------------------------------------------------------------------

// The restarted TeamSpeak gets only these data-free flags from the original command line; never a
// ts3server:// link (it may carry a password), never a path or anything with '='.
QStringList relaunchFlags(const QStringList& originalArguments);

} // namespace upd
