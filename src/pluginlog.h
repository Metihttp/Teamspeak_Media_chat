#pragma once

// 2.2 foundation: the plugin's own log file, <data dir>/logs/tsmedia.log (UTF-8). It holds the same
// lines as TeamSpeak's log, with private values marked (see logtext.h), so the diagnostic info can show
// recent lines, also from before a TeamSpeak restart, without names. Rotates to tsmedia.1.log above
// kMaxFileBytes and keeps that one backup: at most about twice that on disk.
//
// Thread-safe (one mutex). The file is opened on the first line and closed by shutdown(), which
// ts3plugin_shutdown calls last, so nothing of ours keeps it open after the DLL is unloaded.

#include <QString>
#include <QStringList>

namespace plog {

constexpr qint64 kMaxFileBytes = 256 * 1024;
constexpr int    kMaxLineChars = 2000;

// Where lines go from now on (created when needed). Until start() nothing is written.
void start(const QString& directory);
// Closes the file; later writes are dropped until the next start().
void shutdown();

// level: TeamSpeak's LogLevel (0 critical .. 5 devel). marked: the text with its markers.
void write(int level, const QString& marked);

// The last n lines (oldest first), from the backup too when the current file has fewer.
QStringList tail(int n);

QString currentFilePath(); // empty before start()

// One line as written ("2026-11-02T17:03:40.123+03:30 WARN  text"), without the line break. For tests.
QString formatLine(qint64 msecsSinceEpoch, int offsetFromUtcSeconds, int level, const QString& marked);

} // namespace plog
