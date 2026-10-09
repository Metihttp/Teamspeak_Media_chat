#pragma once

// Collects diag::Facts for the diagnostic info: TeamSpeak, the settings and the plugin's own state on
// the GUI thread; Windows, Media Foundation codecs, the cache and the log lines on a worker thread.
// Reads only local state: no network requests, and never server addresses, names or unique ids.

#include <QPointer>
#include <QString>

#include "diagnostics.h"

class ChatIntegration;
class Core;

namespace diag {

// What plugin.cpp hands over.
struct Environment {
    QPointer<Core>            core;
    QPointer<ChatIntegration> chat;
    bool                      mediaFoundationStarted = false; // mf::startup() succeeded
    int                       pluginApi              = 0;
};

// Paths for the worker, read on the GUI thread (TeamSpeak's functions are called there).
struct WorkerInput {
    QString cacheDir;
    QString teamSpeakLogsDir;
};

Facts collectOnGui(const Environment& env, WorkerInput* input);   // GUI thread, fast
void  collectOnWorker(Facts* facts, const WorkerInput& input);    // any thread; usually well under 1 s

// The TS Media lines of the newest TeamSpeak client log in logsDir (at most its last 2 MB are read).
LogTail readTeamSpeakLog(const QString& logsDir, int maxLines);

// Other features add a section to the report (counts and states only, never names). Providers are
// called on the GUI thread while collecting; registering one twice has no effect.
using SectionProvider = Section (*)();
void addSectionProvider(SectionProvider provider);

// Session counters, counted from Core's signals while the plugin runs. Started once at plugin start,
// as a child of Core (it goes with it at shutdown).
void          startSessionStats(Core* core);
SessionCounts sessionCounts();

} // namespace diag
