#pragma once

// 2.6 chat proxy: the proxy password (design 2.2). Windows DPAPI for the current user (CryptProtectData without
// CRYPTPROTECT_LOCAL_MACHINE, no description, CRYPTPROTECT_UI_FORBIDDEN), with the entropy bound to the endpoint
// ("TSMediaChat/proxy-password/v1|<type>|<host>|<port>|<user>"): a password saved for one proxy can't be opened for
// another (a hand-edited host or port makes it unreadable, S7). Stored as "dpapi1:" + base64.
//
// In memory the plaintext only ever sits in a SecretBytes (wiped when it goes) or, between handshakes, under
// CryptProtectMemory(SAME_PROCESS) in a ProtectedSecret (S18). Never in a QString that outlives the call, never in
// Settings, a log, the diagnostics, the helper or WebView2.
//
// crypt32.dll is loaded once from System32 at run time (never linked, never freed).

#include <QByteArray>
#include <QString>

#include "proxyconfig.h"

namespace proxy::protect {

// A move-only byte buffer for a secret; wiped (SecureZeroMemory) when reset or destroyed.
class SecretBytes
{
  public:
    SecretBytes() = default;
    SecretBytes(const char* data, int size) { assign(data, size); }
    ~SecretBytes() { reset(); }
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    SecretBytes(const SecretBytes&)            = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;

    void        assign(const char* data, int size); // copies (the old content is wiped first)
    void        reset();                            // wiped, then freed
    const char* data() const { return m_data; }
    int         size() const { return m_size; }
    bool        isEmpty() const { return m_size == 0; }

  private:
    char* m_data = nullptr;
    int   m_size = 0;
};

// The DPAPI entropy of an endpoint (UTF-8 bytes).
QByteArray entropy(const Endpoint& endpoint);

// Seals password for endpoint: *blob = "dpapi1:<base64>". An empty password seals to an empty blob (no password).
// False when Windows couldn't protect it (nothing is returned then).
bool seal(const QString& password, const Endpoint& endpoint, QByteArray* blob);
bool seal(const SecretBytes& password, const Endpoint& endpoint, QByteArray* blob);

enum class OpenResult {
    Ok,
    Unreadable, // no prefix, bad base64, ERROR_INVALID_DATA (another user or PC, tampered, another endpoint), NTE_BAD_KEY_STATE
    Transient,  // any other CryptUnprotectData error: not decisive, tried again later
};
// Opens a sealed password for endpoint into *out. An empty blob: Ok with an empty *out.
OpenResult open(const QByteArray& sealed, const Endpoint& endpoint, SecretBytes* out);

// A secret kept under CryptProtectMemory(CRYPTPROTECTMEMORY_SAME_PROCESS) between uses: one LSASS round trip per
// setting change instead of one per tunnel. Not thread-safe by itself (the bridge holds its lock).
class ProtectedSecret
{
  public:
    ProtectedSecret() = default;
    ~ProtectedSecret() { clear(); }
    ProtectedSecret(const ProtectedSecret&)            = delete;
    ProtectedSecret& operator=(const ProtectedSecret&) = delete;

    bool set(const SecretBytes& plain);   // copies, pads to the block size, protects; false when Windows can't
    bool reveal(SecretBytes* out) const;  // an unprotected copy
    bool isSet() const { return m_data != nullptr; }
    void clear();                          // wiped and freed

  private:
    char* m_data     = nullptr;
    int   m_capacity = 0; // a multiple of CRYPTPROTECTMEMORY_BLOCK_SIZE
    int   m_size     = 0;
};

// Standard base64 (with padding) of size bytes into out (no QByteArray, no QString: the Basic header and the RFC 1929
// message are built in one fixed buffer that is wiped after the write). The number of characters written, or -1 when
// out is too small.
int base64Encode(const char* data, int size, char* out, int capacity);

// ---- tests ----------------------------------------------------------------------------------------------------------
// The next CryptUnprotectData calls fail with this Windows error (0: real calls). Counts every open() that reached
// CryptUnprotectData. The observer sees each SecretBytes buffer right after it was wiped (before it is freed).
void setUnprotectErrorForTests(unsigned long error);
int  unprotectCalls();
void setWipeObserverForTests(void (*observer)(const char* data, int size));

} // namespace proxy::protect
