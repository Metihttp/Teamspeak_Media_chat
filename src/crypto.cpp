#include "crypto.h"

#include <windows.h>

#include <bcrypt.h>

#include <climits>
#include <cstring>

namespace crypto {

namespace {

constexpr NTSTATUS kStatusSuccess = 0;

BCRYPT_ALG_HANDLE alg(void* handle)
{
    return static_cast<BCRYPT_ALG_HANDLE>(handle);
}

BCRYPT_HASH_HANDLE hashHandle(void* handle)
{
    return static_cast<BCRYPT_HASH_HANDLE>(handle);
}

} // namespace

Sha256::Sha256()
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != kStatusSuccess) {
        m_failed = true;
        return;
    }
    m_alg = algorithm;

    DWORD objectSize = 0;
    ULONG written    = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0) != kStatusSuccess
        || objectSize == 0 || objectSize > 1024 * 1024) {
        m_failed = true;
        return;
    }
    m_object.resize(static_cast<int>(objectSize));

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(m_object.data()), objectSize, nullptr, 0, 0) != kStatusSuccess) {
        m_failed = true;
        return;
    }
    m_hash = hash;
}

Sha256::~Sha256()
{
    if (m_hash)
        BCryptDestroyHash(hashHandle(m_hash));
    if (m_alg)
        BCryptCloseAlgorithmProvider(alg(m_alg), 0);
}

void Sha256::add(const void* data, size_t size)
{
    if (m_failed || m_finished || size == 0)
        return;
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const ULONG chunk = static_cast<ULONG>(size > ULONG_MAX / 2 ? ULONG_MAX / 2 : size);
        // BCryptHashData takes a non-const pointer but does not write through it.
        if (BCryptHashData(hashHandle(m_hash), const_cast<PUCHAR>(bytes), chunk, 0) != kStatusSuccess) {
            m_failed = true;
            return;
        }
        bytes += chunk;
        size -= chunk;
    }
}

QByteArray Sha256::finish()
{
    if (m_failed || m_finished)
        return {};
    m_finished = true;
    QByteArray digest(32, '\0');
    if (BCryptFinishHash(hashHandle(m_hash), reinterpret_cast<PUCHAR>(digest.data()), 32, 0) != kStatusSuccess) {
        m_failed = true;
        return {};
    }
    return digest;
}

QByteArray Sha256::hash(const QByteArray& data)
{
    Sha256 sha;
    sha.add(data);
    return sha.finish();
}

bool ecdsaP256Verify(const unsigned char (&xy)[64], const QByteArray& data, const QByteArray& sig)
{
    if (sig.size() != 64)
        return false;
    const QByteArray digest = Sha256::hash(data);
    if (digest.size() != 32)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != kStatusSuccess)
        return false;

    // BCRYPT_ECCKEY_BLOB {magic 'ECS1', cbKey 32} followed by X and Y.
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB header;
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey   = 32;
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), xy, 64);

    bool              valid = false;
    BCRYPT_KEY_HANDLE key   = nullptr;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, static_cast<ULONG>(sizeof(blob)), 0) == kStatusSuccess) {
        QByteArray signature = sig; // a writable copy: the API takes non-const pointers
        QByteArray hash      = digest;
        const NTSTATUS status = BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(hash.data()), static_cast<ULONG>(hash.size()),
                                                      reinterpret_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0);
        valid = status == kStatusSuccess; // exactly STATUS_SUCCESS, never BCRYPT_SUCCESS()
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
}

} // namespace crypto
