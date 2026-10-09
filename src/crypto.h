#pragma once

// SHA-256 and ECDSA P-256 signature checks through Windows CNG (bcrypt.dll). QtCore only, no widgets,
// so the unit tests and tools use it too.
//
// Nothing here is static: every Sha256 opens its own algorithm handle and closes it in its destructor,
// and ecdsaP256Verify() opens and closes its handles per call. Nothing is left for DllMain to clean up
// when TeamSpeak unloads the plugin.

#include <QByteArray>

#include <cstddef>

namespace crypto {

// Streaming SHA-256. add() any number of times, then finish() once.
class Sha256
{
  public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&)            = delete;
    Sha256& operator=(const Sha256&) = delete;

    // False if CNG could not be set up or a call failed; finish() then returns an empty array.
    bool ok() const { return !m_failed; }

    void add(const void* data, size_t size);
    void add(const QByteArray& data) { add(data.constData(), static_cast<size_t>(data.size())); }

    // The 32-byte digest, or empty on failure. The object can't be used afterwards.
    QByteArray finish();

    static QByteArray hash(const QByteArray& data);

  private:
    void*      m_alg  = nullptr; // BCRYPT_ALG_HANDLE
    void*      m_hash = nullptr; // BCRYPT_HASH_HANDLE
    QByteArray m_object;         // the hash object's memory
    bool       m_failed   = false;
    bool       m_finished = false;
};

// True only if sig (64 bytes, IEEE P1363 r||s) is a valid ECDSA P-256 signature of SHA-256(data)
// under the public key xy (X||Y, 32 bytes each, big-endian). A signature counts only when
// BCryptVerifySignature returns exactly STATUS_SUCCESS: informational statuses and
// STATUS_INVALID_PARAMETER (a malformed signature) are rejections like STATUS_INVALID_SIGNATURE.
bool ecdsaP256Verify(const unsigned char (&xy)[64], const QByteArray& data, const QByteArray& sig);

} // namespace crypto
