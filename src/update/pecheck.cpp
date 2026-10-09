#include "pecheck.h"

#include <QDir>

#include <windows.h>

#include <winver.h>

#include <cstring>
#include <string>

namespace upd::pe {

namespace {

constexpr int kMaxModules   = 512;
constexpr int kMaxFunctions = 20000;
constexpr int kMaxNameBytes = 512;

class Reader
{
  public:
    explicit Reader(const QByteArray& data)
        : m_data(reinterpret_cast<const unsigned char*>(data.constData()))
        , m_size(static_cast<quint64>(data.size()))
    {
    }

    bool has(quint64 offset, quint64 bytes) const { return offset <= m_size && bytes <= m_size - offset; }

    bool u16(quint64 offset, quint16* out) const
    {
        if (!has(offset, 2))
            return false;
        *out = static_cast<quint16>(m_data[offset] | (m_data[offset + 1] << 8));
        return true;
    }

    bool u32(quint64 offset, quint32* out) const
    {
        if (!has(offset, 4))
            return false;
        *out = static_cast<quint32>(m_data[offset]) | (static_cast<quint32>(m_data[offset + 1]) << 8) | (static_cast<quint32>(m_data[offset + 2]) << 16)
             | (static_cast<quint32>(m_data[offset + 3]) << 24);
        return true;
    }

    bool u64(quint64 offset, quint64* out) const
    {
        quint32 low = 0, high = 0;
        if (!u32(offset, &low) || !u32(offset + 4, &high))
            return false;
        *out = (static_cast<quint64>(high) << 32) | low;
        return true;
    }

    // A NUL-terminated ASCII name of at most kMaxNameBytes printable bytes.
    bool name(quint64 offset, QString* out) const
    {
        QByteArray bytes;
        for (quint64 i = 0; i < kMaxNameBytes; ++i) {
            if (!has(offset + i, 1))
                return false;
            const unsigned char c = m_data[offset + i];
            if (c == 0) {
                if (bytes.isEmpty())
                    return false;
                *out = QString::fromLatin1(bytes);
                return true;
            }
            if (c < 0x20 || c > 0x7e)
                return false;
            bytes.append(static_cast<char>(c));
        }
        return false;
    }

  private:
    const unsigned char* m_data;
    quint64              m_size;
};

struct Section {
    quint32 va;
    quint32 virtualSize;
    quint32 rawSize;
    quint32 rawOffset;
};

struct Headers {
    ImageInfo        info;
    quint32          importRva  = 0;
    quint32          importSize = 0;
    QVector<Section> sections;
};

bool setWhy(QString* why, const char* text)
{
    if (why)
        *why = QString::fromLatin1(text);
    return false;
}

bool parseHeaders(const Reader& r, Headers* h, QString* why)
{
    quint16 mz = 0;
    if (!r.u16(0, &mz) || mz != 0x5a4d)
        return setWhy(why, "not a PE file (no MZ header)");
    quint32 peOffset = 0;
    if (!r.u32(0x3c, &peOffset) || peOffset < 0x40 || peOffset > 0x10000)
        return setWhy(why, "bad PE header offset");
    quint32 signature = 0;
    if (!r.u32(peOffset, &signature) || signature != 0x00004550)
        return setWhy(why, "no PE signature");
    const quint64 fileHeader = peOffset + 4ull;
    quint16       numberOfSections = 0, optionalSize = 0, characteristics = 0;
    if (!r.u16(fileHeader, &h->info.machine) || !r.u16(fileHeader + 2, &numberOfSections) || !r.u16(fileHeader + 16, &optionalSize)
        || !r.u16(fileHeader + 18, &characteristics))
        return setWhy(why, "truncated file header");
    if (numberOfSections == 0 || numberOfSections > 96)
        return setWhy(why, "bad section count");
    h->info.isDll = (characteristics & 0x2000) != 0;

    const quint64 optional = fileHeader + 20;
    quint16       magic    = 0;
    if (!r.u16(optional, &magic))
        return setWhy(why, "truncated optional header");
    quint64 countOffset = 0;
    if (magic == 0x10b) {
        countOffset = optional + 92;
    } else if (magic == 0x20b) {
        h->info.is64 = true;
        countOffset  = optional + 108;
    } else {
        return setWhy(why, "bad optional header magic");
    }
    quint32 directories = 0;
    if (!r.u32(countOffset, &directories))
        return setWhy(why, "truncated optional header");
    if (directories >= 2) {
        const quint64 importDir = countOffset + 4 + 8; // data directory 1 (imports)
        if (importDir + 8 > optional + optionalSize || !r.u32(importDir, &h->importRva) || !r.u32(importDir + 4, &h->importSize))
            return setWhy(why, "truncated data directories");
    }

    const quint64 sectionTable = optional + optionalSize;
    for (quint16 i = 0; i < numberOfSections; ++i) {
        const quint64 s = sectionTable + 40ull * i;
        Section       section{};
        if (!r.u32(s + 8, &section.virtualSize) || !r.u32(s + 12, &section.va) || !r.u32(s + 16, &section.rawSize)
            || !r.u32(s + 20, &section.rawOffset))
            return setWhy(why, "truncated section table");
        h->sections.append(section);
    }
    return true;
}

bool rvaToOffset(const Headers& h, quint32 rva, quint64* offset)
{
    for (const Section& s : h.sections) {
        const quint64 span = qMax(s.virtualSize, s.rawSize);
        if (rva >= s.va && rva < s.va + span) {
            const quint64 delta = rva - s.va;
            if (delta >= s.rawSize)
                return false; // in the zero-filled tail: no data in the file
            *offset = s.rawOffset + delta;
            return true;
        }
    }
    return false;
}

using GetFileVersionInfoSizeWFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using GetFileVersionInfoWFn     = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using VerQueryValueWFn          = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

} // namespace

bool readInfo(const QByteArray& image, ImageInfo* info, QString* why)
{
    Headers h;
    if (!parseHeaders(Reader(image), &h, why))
        return false;
    *info = h.info;
    return true;
}

bool readImports(const QByteArray& image, QVector<ImportedModule>* modules, QString* why)
{
    modules->clear();
    const Reader r(image);
    Headers      h;
    if (!parseHeaders(r, &h, why))
        return false;
    if (h.importRva == 0 || h.importSize == 0)
        return true; // imports nothing
    quint64 descriptor = 0;
    if (!rvaToOffset(h, h.importRva, &descriptor))
        return setWhy(why, "import directory outside the file");

    const quint64 thunkSize    = h.info.is64 ? 8 : 4;
    const quint64 ordinalFlag  = h.info.is64 ? 0x8000000000000000ull : 0x80000000ull;
    int           functionCount = 0;
    for (int index = 0;; ++index) {
        if (index >= kMaxModules)
            return setWhy(why, "too many imported modules");
        const quint64 d = descriptor + 20ull * index;
        quint32       originalThunk = 0, nameRva = 0, firstThunk = 0, stamp = 0, chain = 0;
        if (!r.u32(d, &originalThunk) || !r.u32(d + 4, &stamp) || !r.u32(d + 8, &chain) || !r.u32(d + 12, &nameRva) || !r.u32(d + 16, &firstThunk))
            return setWhy(why, "truncated import directory");
        if (originalThunk == 0 && nameRva == 0 && firstThunk == 0 && stamp == 0 && chain == 0)
            break; // the terminating all-zero descriptor

        ImportedModule module;
        quint64        nameOffset = 0;
        if (!rvaToOffset(h, nameRva, &nameOffset) || !r.name(nameOffset, &module.name))
            return setWhy(why, "bad imported module name");

        quint64 thunk = 0;
        if (!rvaToOffset(h, originalThunk ? originalThunk : firstThunk, &thunk))
            return setWhy(why, "bad import thunk table");
        for (int t = 0;; ++t) {
            if (++functionCount > kMaxFunctions)
                return setWhy(why, "too many imported functions");
            quint64 value = 0;
            if (h.info.is64) {
                if (!r.u64(thunk + thunkSize * t, &value))
                    return setWhy(why, "truncated import thunk table");
            } else {
                quint32 v32 = 0;
                if (!r.u32(thunk + thunkSize * t, &v32))
                    return setWhy(why, "truncated import thunk table");
                value = v32;
            }
            if (value == 0)
                break;
            if (value & ordinalFlag) {
                module.ordinals.append(static_cast<int>(value & 0xffff));
                continue;
            }
            quint64 byName = 0;
            QString function;
            if ((value >> 31) != 0 || !rvaToOffset(h, static_cast<quint32>(value), &byName) || !r.name(byName + 2, &function))
                return setWhy(why, "bad imported function name");
            module.functions.append(function);
        }
        modules->append(module);
    }
    return true;
}

QStringList unresolvedImports(const QVector<ImportedModule>& modules, const QStringList& extraDirs)
{
    QStringList missing;
    for (const ImportedModule& module : modules) {
        const std::wstring name   = module.name.toStdWString();
        HMODULE            handle = GetModuleHandleW(name.c_str());
        bool               loaded = false;
        if (!handle) {
            handle = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            loaded = handle != nullptr;
        }
        for (int i = 0; !handle && i < extraDirs.size(); ++i) {
            const std::wstring path = QDir::toNativeSeparators(extraDirs.at(i) + QLatin1Char('/') + module.name).toStdWString();
            handle                  = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            loaded                  = handle != nullptr;
        }
        if (!handle) {
            missing.append(module.name);
            continue;
        }
        for (const QString& function : module.functions) {
            if (!GetProcAddress(handle, function.toLatin1().constData()))
                missing.append(module.name + QLatin1Char('!') + function);
        }
        for (int ordinal : module.ordinals) {
            if (!GetProcAddress(handle, MAKEINTRESOURCEA(ordinal)))
                missing.append(module.name + QLatin1String("!#") + QString::number(ordinal));
        }
        if (loaded)
            FreeLibrary(handle);
    }
    return missing;
}

QString fileVersion(const QString& path)
{
    // version.dll is loaded only here, so it doesn't become an import of the plugin.
    HMODULE library = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return QString();
    QString result;
    const auto sizeFn  = reinterpret_cast<GetFileVersionInfoSizeWFn>(reinterpret_cast<void*>(GetProcAddress(library, "GetFileVersionInfoSizeW")));
    const auto infoFn  = reinterpret_cast<GetFileVersionInfoWFn>(reinterpret_cast<void*>(GetProcAddress(library, "GetFileVersionInfoW")));
    const auto queryFn = reinterpret_cast<VerQueryValueWFn>(reinterpret_cast<void*>(GetProcAddress(library, "VerQueryValueW")));
    if (sizeFn && infoFn && queryFn) {
        const std::wstring file  = QDir::toNativeSeparators(path).toStdWString();
        DWORD              dummy = 0;
        const DWORD        size  = sizeFn(file.c_str(), &dummy);
        if (size > 0 && size < 1024 * 1024) {
            QByteArray data(static_cast<int>(size), '\0');
            void*      fixed  = nullptr;
            UINT       length = 0;
            if (infoFn(file.c_str(), 0, size, data.data()) && queryFn(data.constData(), L"\\", &fixed, &length) && fixed
                && length >= sizeof(VS_FIXEDFILEINFO)) {
                const auto* info = static_cast<const VS_FIXEDFILEINFO*>(fixed);
                if (info->dwSignature == 0xfeef04bd)
                    result = QString::number(HIWORD(info->dwFileVersionMS)) + QLatin1Char('.') + QString::number(LOWORD(info->dwFileVersionMS))
                           + QLatin1Char('.') + QString::number(HIWORD(info->dwFileVersionLS));
            }
        }
    }
    FreeLibrary(library);
    return result;
}

} // namespace upd::pe
