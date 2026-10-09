#include "updatefiles.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>

#include <algorithm>
#include <vector>

#include "crypto.h"

namespace upd {

namespace {

QString fromWide(const wchar_t* text, size_t length)
{
    return QString::fromWCharArray(text, static_cast<int>(length));
}

std::wstring wide(const char* text)
{
    return QString::fromLatin1(text).toStdWString();
}

// Any address inside this module.
void moduleAnchor() {}

} // namespace

QString Layout::ownModulePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module))
        return QString();
    std::vector<wchar_t> buffer(MAX_PATH);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return QString();
        if (length < buffer.size())
            return QDir::fromNativeSeparators(fromWide(buffer.data(), length));
        buffer.resize(buffer.size() * 2); // truncated: grow (long paths)
    }
    return QString();
}

Layout Layout::forRunningPlugin(const QString& dataDir)
{
    Layout layout;
    layout.pluginsDir = QFileInfo(ownModulePath()).absolutePath();
    layout.updateDir  = QDir::cleanPath(dataDir + QLatin1String("/update"));
    layout.configDir  = QDir::cleanPath(dataDir + QLatin1String("/../.."));
    return layout;
}

QStringList pluginFileNames()
{
    return {targetFileName(FileKind::Plugin, Arch::Win64), targetFileName(FileKind::Plugin, Arch::Win32)};
}

namespace fs {

std::wstring native(const QString& path)
{
    QString p = QDir::toNativeSeparators(QDir::cleanPath(path));
    // Long absolute paths need the \\?\ form for the W APIs (MAX_PATH is 260 without it).
    if (p.size() >= 240 && p.size() > 2 && p.at(1) == QLatin1Char(':'))
        p.prepend(QLatin1String("\\\\?\\"));
    return p.toStdWString();
}

bool exists(const QString& path)
{
    return GetFileAttributesW(native(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDir(const QString& path)
{
    const DWORD attributes = GetFileAttributesW(native(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

qint64 fileSize(const QString& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return static_cast<qint64>((static_cast<quint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
}

bool ensureDir(const QString& path)
{
    if (isDir(path))
        return true;
    const QString parent = QFileInfo(QDir::cleanPath(path)).absolutePath();
    if (parent != QDir::cleanPath(path) && !isDir(parent) && !ensureDir(parent))
        return false;
    return CreateDirectoryW(native(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeFile(const QString& path)
{
    const std::wstring p = native(path);
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(p.c_str()) != 0;
}

QStringList entries(const QString& dir, bool dirs)
{
    QStringList      result;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = native(dir + QLatin1String("/*"));
    HANDLE             find    = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return result;
    do {
        const QString name = QString::fromWCharArray(data.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue; // never follow links out of our folders
        const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory == dirs)
            result.append(name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return result;
}

bool removeTree(const QString& path)
{
    if (!isDir(path))
        return removeFile(path);
    for (const QString& name : entries(path, false))
        removeFile(path + QLatin1Char('/') + name);
    for (const QString& name : entries(path, true))
        removeTree(path + QLatin1Char('/') + name);
    RemoveDirectoryW(native(path).c_str());
    return !exists(path);
}

bool moveFile(const QString& from, const QString& to, bool replace, unsigned long* error)
{
    const std::wstring source = native(from);
    const std::wstring target = native(to);
    const DWORD        flags  = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    DWORD              last   = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(source.c_str(), target.c_str(), flags)) {
            if (error)
                *error = 0;
            return true;
        }
        last = GetLastError();
        if (last != ERROR_SHARING_VIOLATION && last != ERROR_ACCESS_DENIED && last != ERROR_LOCK_VIOLATION)
            break;
        Sleep(200);
    }
    if (error)
        *error = last;
    return false;
}

QByteArray sha256(const QString& path, qint64* size, qint64 maxBytes)
{
    if (size)
        *size = -1;
    HANDLE file = CreateFileW(native(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    crypto::Sha256 sha;
    QByteArray     buffer(64 * 1024, '\0');
    qint64         total = 0;
    bool           ok    = sha.ok();
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        total += read;
        if (total > maxBytes) {
            ok = false;
            break;
        }
        sha.add(buffer.constData(), read);
    }
    CloseHandle(file);
    if (!ok)
        return {};
    if (size)
        *size = total;
    return sha.finish();
}

bool readAll(const QString& path, QByteArray* data, qint64 maxBytes)
{
    data->clear();
    HANDLE file = CreateFileW(native(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool          ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= maxBytes;
    if (ok) {
        data->resize(static_cast<int>(size.QuadPart));
        qint64 done = 0;
        while (ok && done < size.QuadPart) {
            DWORD read = 0;
            ok         = ReadFile(file, data->data() + done, static_cast<DWORD>(qMin<qint64>(size.QuadPart - done, 1 << 20)), &read, nullptr) && read > 0;
            done += read;
        }
    }
    CloseHandle(file);
    if (!ok)
        data->clear();
    return ok;
}

bool writeAll(const QString& path, const QByteArray& data)
{
    const QString tmp  = path + QLatin1String(".tmp");
    HANDLE        file = CreateFileW(native(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool  ok      = data.isEmpty() || (WriteFile(file, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr) && written == static_cast<DWORD>(data.size()));
    ok            = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    if (!ok || !moveFile(tmp, path, true)) {
        removeFile(tmp);
        return false;
    }
    return true;
}

bool sameVolume(const QString& a, const QString& b)
{
    wchar_t va[MAX_PATH + 1] = {};
    wchar_t vb[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(native(a).c_str(), va, MAX_PATH) || !GetVolumePathNameW(native(b).c_str(), vb, MAX_PATH))
        return false;
    return QString::fromWCharArray(va).compare(QString::fromWCharArray(vb), Qt::CaseInsensitive) == 0;
}

quint64 freeBytes(const QString& dir)
{
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(native(dir).c_str(), &available, nullptr, nullptr))
        return 0;
    return available.QuadPart;
}

} // namespace fs

// ---- StateFile -----------------------------------------------------------------------------------

QString StateFile::value(const char* section, const QString& key) const
{
    wchar_t            buffer[512] = {};
    const std::wstring s           = wide(section);
    const std::wstring k           = key.toStdWString();
    const DWORD length = GetPrivateProfileStringW(s.c_str(), k.c_str(), L"", buffer, 512, fs::native(m_path).c_str());
    // Only printable ASCII is ever written; anything else is treated as missing.
    QString result = fromWide(buffer, length);
    for (QChar c : result) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return QString();
    }
    return result;
}

int StateFile::intValue(const char* section, const char* key, int fallback, int min, int max) const
{
    bool      ok     = false;
    const int number = value(section, key).toInt(&ok);
    return ok ? qBound(min, number, max) : fallback;
}

QDateTime StateFile::timeValue(const char* section, const char* key) const
{
    QDateTime time = QDateTime::fromString(value(section, key), Qt::ISODate);
    if (time.isValid())
        time = time.toUTC();
    return time;
}

Version StateFile::versionValue(const char* section, const char* key) const
{
    return Version::parse(value(section, key));
}

bool StateFile::setValue(const char* section, const QString& key, const QString& value)
{
    fs::ensureDir(QFileInfo(m_path).absolutePath());
    const std::wstring s = wide(section);
    const std::wstring k = key.toStdWString();
    const std::wstring v = value.toStdWString();
    return WritePrivateProfileStringW(s.c_str(), k.c_str(), v.c_str(), fs::native(m_path).c_str()) != 0;
}

bool StateFile::setInt(const char* section, const char* key, int value)
{
    return setValue(section, key, QString::number(value));
}

bool StateFile::setTime(const char* section, const char* key, const QDateTime& utc)
{
    if (!utc.isValid())
        return remove(section, key);
    return setValue(section, key, utc.toUTC().toString(Qt::ISODate));
}

bool StateFile::remove(const char* section, const char* key)
{
    const std::wstring s = wide(section);
    const std::wstring k = wide(key);
    return WritePrivateProfileStringW(s.c_str(), k.c_str(), nullptr, fs::native(m_path).c_str()) != 0;
}

bool StateFile::clearSection(const char* section)
{
    const std::wstring s = wide(section);
    return WritePrivateProfileStringW(s.c_str(), nullptr, nullptr, fs::native(m_path).c_str()) != 0;
}

QSet<int> StateFile::revokedKeys() const
{
    QSet<int> ids;
    for (const QString& part : value("trust", "revoked").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool      ok = false;
        const int id = part.trimmed().toInt(&ok);
        if (ok && id >= 1 && id <= 255)
            ids.insert(id);
    }
    return ids;
}

bool StateFile::addRevokedKeys(const QSet<int>& ids)
{
    QSet<int> all = revokedKeys();
    all.unite(ids);
    QList<int> sorted = all.values();
    std::sort(sorted.begin(), sorted.end());
    QStringList parts;
    for (int id : sorted)
        parts.append(QString::number(id));
    return setValue("trust", "revoked", parts.join(QLatin1Char(',')));
}

} // namespace upd
