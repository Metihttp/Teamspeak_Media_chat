#include "protect.h"

#include <windows.h>

#include <wincrypt.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace proxy::protect {

namespace {

constexpr char kPrefix[] = "dpapi1:";
constexpr int  kPrefixChars = 7;
constexpr int  kBlock       = CRYPTPROTECTMEMORY_BLOCK_SIZE;
#ifndef NTE_BAD_KEY_STATE
constexpr LONG NTE_BAD_KEY_STATE = static_cast<LONG>(0x8009000BL);
#endif

using ProtectDataFn     = BOOL(WINAPI*)(DATA_BLOB*, LPCWSTR, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
using UnprotectDataFn   = BOOL(WINAPI*)(DATA_BLOB*, LPWSTR*, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
using ProtectMemoryFn   = BOOL(WINAPI*)(LPVOID, DWORD, DWORD);
using UnprotectMemoryFn = BOOL(WINAPI*)(LPVOID, DWORD, DWORD);

struct Crypt32 {
    HMODULE           dll             = nullptr;
    ProtectDataFn     protectData     = nullptr;
    UnprotectDataFn   unprotectData   = nullptr;
    ProtectMemoryFn   protectMemory   = nullptr;
    UnprotectMemoryFn unprotectMemory = nullptr;

    Crypt32()
    {
        dll = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
            return;
        protectData     = reinterpret_cast<ProtectDataFn>(reinterpret_cast<void*>(GetProcAddress(dll, "CryptProtectData")));
        unprotectData   = reinterpret_cast<UnprotectDataFn>(reinterpret_cast<void*>(GetProcAddress(dll, "CryptUnprotectData")));
        protectMemory   = reinterpret_cast<ProtectMemoryFn>(reinterpret_cast<void*>(GetProcAddress(dll, "CryptProtectMemory")));
        unprotectMemory = reinterpret_cast<UnprotectMemoryFn>(reinterpret_cast<void*>(GetProcAddress(dll, "CryptUnprotectMemory")));
    }
};

// Loaded once, never freed (the winhttp pattern): nothing of it runs at unload.
const Crypt32& crypt()
{
    static Crypt32* api = new Crypt32;
    return *api;
}

std::atomic<unsigned long> g_unprotectError{0};
std::atomic<int>           g_unprotectCalls{0};
std::atomic<void (*)(const char*, int)> g_wipeObserver{nullptr};

void wipe(void* data, size_t size)
{
    if (data && size)
        SecureZeroMemory(data, size);
}

// A QByteArray we own alone (a fresh copy): wiped in place before it goes.
void wipe(QByteArray* bytes)
{
    if (bytes && !bytes->isEmpty())
        wipe(bytes->data(), static_cast<size_t>(bytes->size()));
    if (bytes)
        bytes->clear();
}

int decodeChar(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

// Strict standard base64 (padding required, nothing else allowed).
bool decodeStrict(const char* text, int size, QByteArray* out)
{
    out->clear();
    if (size <= 0 || size % 4 != 0)
        return false;
    out->reserve(size / 4 * 3);
    for (int i = 0; i < size; i += 4) {
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                if (i + 4 != size || j < 2)
                    return false;
                v[j] = 0;
                ++pad;
                continue;
            }
            if (pad > 0)
                return false;
            v[j] = decodeChar(c);
            if (v[j] < 0)
                return false;
        }
        const quint32 n = (static_cast<quint32>(v[0]) << 18) | (static_cast<quint32>(v[1]) << 12) | (static_cast<quint32>(v[2]) << 6) | static_cast<quint32>(v[3]);
        out->append(static_cast<char>((n >> 16) & 0xff));
        if (pad < 2)
            out->append(static_cast<char>((n >> 8) & 0xff));
        if (pad < 1)
            out->append(static_cast<char>(n & 0xff));
    }
    return true;
}

bool sealBytes(const char* data, int size, const Endpoint& endpoint, QByteArray* blob)
{
    blob->clear();
    if (size <= 0)
        return true;
    const Crypt32& api = crypt();
    if (!api.protectData)
        return false;
    QByteArray extra = entropy(endpoint);
    DATA_BLOB  in{static_cast<DWORD>(size), reinterpret_cast<BYTE*>(const_cast<char*>(data))};
    DATA_BLOB  ent{static_cast<DWORD>(extra.size()), reinterpret_cast<BYTE*>(extra.data())};
    DATA_BLOB  out{};
    // No description (it would be stored in clear), never CRYPTPROTECT_LOCAL_MACHINE.
    if (!api.protectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) || !out.pbData)
        return false;
    QByteArray raw(reinterpret_cast<const char*>(out.pbData), static_cast<int>(out.cbData));
    wipe(out.pbData, out.cbData);
    LocalFree(out.pbData);
    *blob = QByteArray(kPrefix) + raw.toBase64(); // the sealed blob is not a secret
    return true;
}

} // namespace

// ---- SecretBytes ------------------------------------------------------------------------------------------------------

SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr))
    , m_size(std::exchange(other.m_size, 0))
{
}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept
{
    if (this != &other) {
        reset();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

void SecretBytes::assign(const char* data, int size)
{
    reset();
    if (!data || size <= 0)
        return;
    m_data = new char[static_cast<size_t>(size)];
    memcpy(m_data, data, static_cast<size_t>(size));
    m_size = size;
}

void SecretBytes::reset()
{
    if (m_data) {
        wipe(m_data, static_cast<size_t>(m_size));
        if (auto observer = g_wipeObserver.load())
            observer(m_data, m_size);
        delete[] m_data;
    }
    m_data = nullptr;
    m_size = 0;
}

// ---- DPAPI -------------------------------------------------------------------------------------------------------------

QByteArray entropy(const Endpoint& endpoint)
{
    QByteArray e("TSMediaChat/proxy-password/v1|");
    e += endpoint.type == Type::Http ? "http" : "socks5";
    e += '|';
    e += endpoint.host.toUtf8();
    e += '|';
    e += QByteArray::number(endpoint.port);
    e += '|';
    e += endpoint.user.toUtf8();
    return e;
}

bool seal(const QString& password, const Endpoint& endpoint, QByteArray* blob)
{
    QByteArray  utf8 = password.toUtf8();
    const bool  ok   = sealBytes(utf8.constData(), utf8.size(), endpoint, blob);
    wipe(&utf8);
    return ok;
}

bool seal(const SecretBytes& password, const Endpoint& endpoint, QByteArray* blob)
{
    return sealBytes(password.data(), password.size(), endpoint, blob);
}

OpenResult open(const QByteArray& sealed, const Endpoint& endpoint, SecretBytes* out)
{
    out->reset();
    if (sealed.isEmpty())
        return OpenResult::Ok;
    if (!sealed.startsWith(kPrefix))
        return OpenResult::Unreadable;
    QByteArray raw;
    if (!decodeStrict(sealed.constData() + kPrefixChars, sealed.size() - kPrefixChars, &raw) || raw.isEmpty())
        return OpenResult::Unreadable;
    const Crypt32& api = crypt();
    if (!api.unprotectData)
        return OpenResult::Transient;
    ++g_unprotectCalls;
    if (const unsigned long injected = g_unprotectError.load()) {
        const LONG code = static_cast<LONG>(injected);
        return injected == ERROR_INVALID_DATA || code == NTE_BAD_KEY_STATE ? OpenResult::Unreadable : OpenResult::Transient;
    }
    QByteArray extra = entropy(endpoint);
    DATA_BLOB  in{static_cast<DWORD>(raw.size()), reinterpret_cast<BYTE*>(raw.data())};
    DATA_BLOB  ent{static_cast<DWORD>(extra.size()), reinterpret_cast<BYTE*>(extra.data())};
    DATA_BLOB  plain{};
    if (!api.unprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        const DWORD error = GetLastError();
        return error == ERROR_INVALID_DATA || static_cast<LONG>(error) == NTE_BAD_KEY_STATE ? OpenResult::Unreadable : OpenResult::Transient;
    }
    out->assign(reinterpret_cast<const char*>(plain.pbData), static_cast<int>(plain.cbData));
    wipe(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return OpenResult::Ok;
}

// ---- ProtectedSecret --------------------------------------------------------------------------------------------------

bool ProtectedSecret::set(const SecretBytes& plain)
{
    clear();
    if (plain.isEmpty())
        return true;
    const Crypt32& api = crypt();
    if (!api.protectMemory || !api.unprotectMemory)
        return false;
    const int capacity = (plain.size() + kBlock - 1) / kBlock * kBlock;
    char*     buffer   = new char[static_cast<size_t>(capacity)];
    memset(buffer, 0, static_cast<size_t>(capacity));
    memcpy(buffer, plain.data(), static_cast<size_t>(plain.size()));
    if (!api.protectMemory(buffer, static_cast<DWORD>(capacity), CRYPTPROTECTMEMORY_SAME_PROCESS)) {
        wipe(buffer, static_cast<size_t>(capacity));
        delete[] buffer;
        return false;
    }
    m_data     = buffer;
    m_capacity = capacity;
    m_size     = plain.size();
    return true;
}

bool ProtectedSecret::reveal(SecretBytes* out) const
{
    out->reset();
    if (!m_data)
        return true;
    const Crypt32& api  = crypt();
    char*          copy = new char[static_cast<size_t>(m_capacity)];
    memcpy(copy, m_data, static_cast<size_t>(m_capacity));
    const bool ok = api.unprotectMemory && api.unprotectMemory(copy, static_cast<DWORD>(m_capacity), CRYPTPROTECTMEMORY_SAME_PROCESS);
    if (ok)
        out->assign(copy, m_size);
    wipe(copy, static_cast<size_t>(m_capacity));
    delete[] copy;
    return ok;
}

void ProtectedSecret::clear()
{
    if (m_data) {
        wipe(m_data, static_cast<size_t>(m_capacity));
        delete[] m_data;
    }
    m_data     = nullptr;
    m_capacity = 0;
    m_size     = 0;
}

// ---- base64 ------------------------------------------------------------------------------------------------------------

int base64Encode(const char* data, int size, char* out, int capacity)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (size < 0)
        return -1;
    const int need = (size + 2) / 3 * 4;
    if (need > capacity)
        return -1;
    int o = 0;
    for (int i = 0; i < size; i += 3) {
        const quint32 b0 = static_cast<quint8>(data[i]);
        const quint32 b1 = i + 1 < size ? static_cast<quint8>(data[i + 1]) : 0u;
        const quint32 b2 = i + 2 < size ? static_cast<quint8>(data[i + 2]) : 0u;
        const quint32 n  = (b0 << 16) | (b1 << 8) | b2;
        out[o++]         = kAlphabet[(n >> 18) & 63];
        out[o++]         = kAlphabet[(n >> 12) & 63];
        out[o++]         = i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
        out[o++]         = i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return o;
}

// ---- tests --------------------------------------------------------------------------------------------------------------

void setUnprotectErrorForTests(unsigned long error)
{
    g_unprotectError.store(error);
}

int unprotectCalls()
{
    return g_unprotectCalls.load();
}

void setWipeObserverForTests(void (*observer)(const char* data, int size))
{
    g_wipeObserver.store(observer);
}

} // namespace proxy::protect
