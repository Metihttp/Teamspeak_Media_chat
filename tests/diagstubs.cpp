// 2.7 F3: the Settings window is tested whole (tst_settingsui), and its Troubleshooting card links the Diagnostic info
// window (diagnosticsdialog.cpp). That window's collectors (diagnosticscollect.cpp) read the whole chat integration, which
// the tests don't build; the tests never open the window (no opener is registered), so these stand-ins are never called.

#include "diagnosticscollect.h"

namespace diag {

Facts collectOnGui(const Environment&, WorkerInput*)
{
    return Facts();
}

void collectOnWorker(Facts*, const WorkerInput&)
{
}

} // namespace diag
