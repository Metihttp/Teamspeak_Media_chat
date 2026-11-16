#include "delayload.h"

#include <windows.h>

#include <delayimp.h>

namespace {

// C-style on purpose: no C++ object with a destructor may live in a function with __try (C2712), and nothing here may
// unwind through the delay-load helper's exception.
int loadAllImports(const char* dllName)
{
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll(dllName)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace

namespace delayload {

bool bindLoaded(const wchar_t* moduleName, const char* dllName)
{
    if (!moduleName || !dllName || !GetModuleHandleW(moduleName))
        return false; // never load a copy of our own: only the one the process (TeamSpeak) has
    return loadAllImports(dllName) == 1;
}

bool bindQtSql()
{
    static int bound = -1; // this DLL's lifetime: a reload of the plugin asks again
    if (bound < 0) {
#ifdef _DEBUG // the Debug configuration links Qt's debug DLLs
        bound = bindLoaded(L"Qt5Sqld.dll", "Qt5Sqld.dll") ? 1 : 0;
#else
        bound = bindLoaded(L"Qt5Sql.dll", "Qt5Sql.dll") ? 1 : 0;
#endif
    }
    return bound == 1;
}

} // namespace delayload
