#pragma once

// "Copy diagnostic info": the plain-text report a bug report asks for (the "Diagnostic info" field of
// .github/ISSUE_TEMPLATE/bug_report.yml). Pure logic, QtCore only, so the unit tests cover the
// redaction and the text. Collecting the facts (Win32, TeamSpeak, Media Foundation) is in
// diagnosticscollect.cpp, the window in diagnosticsdialog.cpp.
//
// Privacy: the report never contains server addresses, server or channel names, unique ids or
// nicknames. File names and local paths in log lines become <file 1>, <path 1>, … unless the user
// asks to include them; then the Windows user folder still shows as %USERPROFILE%.

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "settings.h"

namespace diag {

// ---- log lines ----------------------------------------------------------------------------------

// Markers of the structured log (ts3::log with file() / local() / pub()): a private value is written
// as U+E000, a kind letter, the value, U+E001. Kinds: 'f' a file name or remote path, 'l' a local
// path, 'n' any other name. 'n' and unknown kinds are never shown, not even with names included.
constexpr ushort kMarkBegin = 0xE000;
constexpr ushort kMarkEnd   = 0xE001;

// The marked form of one value (stray markers inside it are removed).
QString mark(char kind, const QString& value);

struct LogLine {
    QString time;  // as the report shows it: "11-02 17:03:40" (may be empty)
    QString level; // "INFO", "WARN", "ERROR", "DEBUG", "DEVEL", "CRIT"
    QString text;  // the message, possibly with markers
};

struct LogTail {
    enum class Source { None, PluginLog, TeamSpeakLog };

    QVector<LogLine> lines;           // the most recent ones, oldest first
    int              total   = 0;     // TS Media lines the source holds ("last 40 of 212")
    bool             partial = false; // only the end of a large log was read: total is a minimum
    bool             marked  = false; // the source tags private values: unmarked text is public
    Source           source  = Source::None;
    QString          problem; // why there are no lines (shown instead of them)
};

// Where the report's log lines come from. The plugin's own log file (structured, marked lines) is
// wired here by the integration of the structured ts3::log API; until then (or when it returns no
// source) the report falls back to the TS Media lines of TeamSpeak's client log, which are unmarked
// and redacted by their wording (markLegacy). Called on a worker thread: must be thread-safe.
using LogTailProvider = LogTail (*)(int maxLines);
void            setLogTailProvider(LogTailProvider provider); // nullptr: only the TeamSpeak log
LogTailProvider logTailProvider();

// One line of the plugin's own log: "2026-11-02T17:03:40.123+03:30 WARN  text".
LogLine parsePluginLogLine(const QString& line);

// The tail of the plugin's own log from its raw lines (oldest first) and how many lines it holds, for
// the provider: setLogTailProvider([](int n) { return diag::pluginLogTail(plog::tail(n), total); }).
// Without lines it reports Source::None, so the TeamSpeak log is used instead.
LogTail pluginLogTail(const QStringList& rawLines, int totalLines);

// The TS Media lines (channel "TSMedia") of a TeamSpeak client log, at most maxLines, oldest first.
// Lines of every other channel (they name servers, channels and people) are never taken.
LogTail parseTeamSpeakLog(const QByteArray& utf8, int maxLines);

// An unmarked message (TeamSpeak's log, or a line written before the structured log) with its
// private parts marked: the plugin's own messages by their wording, anything else by pattern. Fails
// closed: what looks like a path, a file name or an address is marked.
QString markLegacy(const QString& text);

// Applied to every line, marked or not: curly-quoted text (“…”, which plugin messages put around
// names), absolute local paths, IP addresses and base64 unique ids are marked.
QString markSafetyNet(const QString& text);

// Replaces the marked values of one report with <file N>, <path N> and <name N>, numbered by first
// appearance, so the same file keeps its number across lines. An unbalanced marker hides the rest of
// the line. With includeNames, 'f' and 'l' values are shown, the home folder as %USERPROFILE%.
class Redactor
{
  public:
    Redactor(bool includeNames, const QString& homeDir);

    QString apply(const QString& marked);

  private:
    QString token(char kind, const QString& value, bool reveal);

    bool               m_includeNames;
    QString            m_home;
    QMap<QString, int> m_numbers; // kind + value -> number
    QMap<char, int>    m_counts;  // kind -> numbers handed out
};

// ---- the facts ------------------------------------------------------------------------------------

struct Codec {
    QString label;            // "H.264"
    bool    found    = false;
    bool    hardware = false; // a hardware (GPU) transform is among them (shown for encoders)
};

struct CodecReport {
    bool            checked = false; // false: Media Foundation is missing, nothing was asked
    QVector<Codec>  videoDecoders;
    QVector<Codec>  audioDecoders;
    QVector<Codec>  encoders;
    QString         error; // HRESULT text when the check failed ("0x80004005")
    qint64          elapsedMs = 0;
};

// Session counters (counts only).
struct SessionCounts {
    qint64          sessionMs        = -1; // since the plugin started; -1 unknown
    int             downloadsOk      = 0;
    int             downloadsFailed  = 0;
    QMap<int, int>  downloadErrors;        // MediaError -> failures
    int             uploadsOk        = 0;
    int             uploadsFailed    = 0;
    int             uploadsCanceled  = 0;
};

// Lines another feature adds to the report (counts and states only, never names), under a heading
// of its own.
struct Section {
    QString     title;
    QStringList lines;
};

struct Facts {
    QDateTime created; // local time

    // Versions
    QString pluginName;    // "TS Media chat"
    QString pluginVersion; // "2.2.0"
    int     pluginBits = 0; // 64 / 32: also TeamSpeak's (a plugin only loads into its own kind)
    QString qtRuntime;
    QString qtBuilt;
    int     pluginApi = 0;
    QString teamSpeakVersion; // "3.6.2 [Build: 1695203293]"
    bool    teamSpeakVersionFromLib = false; // from the client library (no server tab connected)
    int     configFolder = 0; // 0 unknown, 1 standard (%APPDATA%), 2 portable

    // Windows (HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion)
    QString windowsProduct;        // ProductName, "Windows 10 Pro" also on Windows 11
    QString windowsDisplayVersion; // "25H2" (DisplayVersion, else ReleaseId)
    QString windowsEdition;        // EditionID, "Professional", "ProfessionalN"
    int     windowsBuild = 0;
    int     windowsUbr   = 0;
    QString nativeArch; // "x64", "ARM64", "x86"
    bool    wow64 = false; // a 32-bit TeamSpeak on 64-bit Windows

    // Media
    bool        mediaFoundationPresent = false; // mfplat.dll and mfreadwrite.dll exist
    bool        mediaFoundationStarted = false; // the plugin's MFStartup succeeded
    CodecReport codecs;
    int         videoDevice = 0; // mf::VideoDevice

    // Display
    int    scalePercent = 0;
    double devicePixelRatio = 0;
    int    screens = 0;
    bool   animations = true;
    int    theme = 0; // 0 unknown, 1 light, 2 dark

    // Connections (no addresses or names: versions only)
    int         connections = 0;
    QStringList serverVersions; // "3.13.7 [Build: 1655727713] on Linux"
    int         chatViews  = -1; // -1 unknown
    int         chatInputs = -1;

    // Settings and activity
    Settings      settings;
    SessionCounts session;
    bool          cacheKnown = false;
    quint64       cacheBytes = 0;
    int           cacheFiles = 0;

    QVector<Section> extra;
    LogTail          log;
};

// Facts that come from "Windows 10 Pro", 26200, "25H2": "Windows 11 Pro 25H2" (ProductName still
// says Windows 10 on Windows 11).
QString windowsName(const QString& product, int build, const QString& displayVersion);
bool    isNEdition(const QString& editionId); // "ProfessionalN", "EnterpriseSN"

// Values from TeamSpeak or a server (untrusted): printable ASCII only, at most maxLength characters.
QString cleanValue(const QString& value, int maxLength);

constexpr int kMaxLogLines     = 40;
constexpr int kMaxLineLength   = 300;
constexpr int kMaxReportLength = 12000; // GitHub's issue body allows 65,536

// The report as the dialog shows and copies it.
QString format(const Facts& facts, bool includeNames, const QString& homeDir);

// The bug report form with only the version fields filled in (field ids of bug_report.yml).
QUrl bugReportUrl(const Facts& facts);

} // namespace diag
