#pragma once

// Checks on a downloaded DLL or exe before it replaces anything: the CPU it was built for, and whether
// every function it imports exists in this TeamSpeak process. A new version that imports a symbol a
// friend's PC doesn't have would fail to load ("Failed to load plugin"), and a plugin that never runs
// can never update itself again, so such an update is refused and left to a manual install.
//
// The file is parsed as data; it is never loaded or run. Every offset is bounds-checked.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace upd::pe {

struct ImageInfo {
    quint16 machine = 0;     // IMAGE_FILE_MACHINE_AMD64 0x8664, I386 0x14c
    bool    isDll   = false;
    bool    is64    = false; // PE32+
};

struct ImportedModule {
    QString     name;       // as written in the import table, e.g. "Qt5Core.dll"
    QStringList functions;  // by name
    QVector<int> ordinals;  // by ordinal
};

// False (with *why) if image isn't a well-formed PE file.
bool readInfo(const QByteArray& image, ImageInfo* info, QString* why = nullptr);
// The normal import directory (not delay-load imports: those may be missing, by design).
bool readImports(const QByteArray& image, QVector<ImportedModule>* modules, QString* why = nullptr);

// Every import that doesn't resolve, as "Module.dll!Function" or "Module.dll!#12"; empty if all do.
// A module is looked up among those already loaded into this process (GetModuleHandle), then in
// System32 (LoadLibraryEx with LOAD_LIBRARY_SEARCH_SYSTEM32, released again afterwards), then in
// extraDirs (tests only). It must match the architecture of this process.
QStringList unresolvedImports(const QVector<ImportedModule>& modules, const QStringList& extraDirs = {});

// The FILEVERSION of a file's version resource as "2.1.0" (the fourth part is ignored), or empty.
QString fileVersion(const QString& path);

} // namespace upd::pe
