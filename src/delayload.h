#pragma once

// 2.7 F2 (design H4): the guard in front of the delay-loaded Qt5Sql.dll (CMakeLists.txt tsmedia_delay_load_qtsql).
// TeamSpeak 3.6.2 ships Qt5Sql.dll and loads it itself for its settings.db; the plugin never loads a Qt5Sql of its own
// from elsewhere. bindQtSql() binds every Qt5Sql import of this DLL at once, only when TeamSpeak's Qt5Sql is already in
// the process, inside a structured exception handler (a missing export raises one): false means no QtSql call may be
// made in this session (the contacts list is Unavailable). The answer is kept for the process's life of this DLL.

namespace delayload {

// TeamSpeak's (the process's) Qt5Sql.dll is loaded and every import of it is bound.
bool bindQtSql();

// The inner guard (tests): moduleName must already be loaded in the process; then every delay-load import of dllName
// (as the import table names it) is bound. False, never an exception, when it isn't loaded or a symbol is missing.
bool bindLoaded(const wchar_t* moduleName, const char* dllName);

} // namespace delayload
