#pragma once

// tsmedia-update.json: the signed manifest every release publishes (docs/UPDATES.md). Pure QtCore.
//
// Outer file (at most 64 KiB):
//   {"format":1,"key":1,"payload":"<base64 of the payload JSON>","sig":"<base64 of 64 bytes r||s>"}
// The signature is ECDSA P-256 over SHA-256 of the exact payload bytes, so no JSON canonicalisation
// is involved. The payload is parsed only after the signature checks out, and is still validated
// field by field. Format 1 is frozen: unknown fields are ignored so later releases can add some.

#include <QByteArray>
#include <QDate>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace upd {

// major.minor.patch, digits only (1-3 each, no leading zeros), no suffix: pre-releases never match.
struct Version {
    int major = -1;
    int minor = 0;
    int patch = 0;

    bool           isValid() const { return major >= 0; }
    QString        toString() const;
    static Version parse(const QString& text); // invalid Version() on anything else

    friend bool operator==(const Version& a, const Version& b) { return a.major == b.major && a.minor == b.minor && a.patch == b.patch; }
    friend bool operator!=(const Version& a, const Version& b) { return !(a == b); }
    friend bool operator<(const Version& a, const Version& b)
    {
        if (a.major != b.major)
            return a.major < b.major;
        if (a.minor != b.minor)
            return a.minor < b.minor;
        return a.patch < b.patch;
    }
    friend bool operator>(const Version& a, const Version& b) { return b < a; }
    friend bool operator<=(const Version& a, const Version& b) { return !(b < a); }
    friend bool operator>=(const Version& a, const Version& b) { return !(a < b); }
};

enum class Arch { Win64, Win32 };
enum class FileKind { Plugin, Helper };

Arch    runningArch(); // of this build (sizeof(void*))
QString archName(Arch arch); // "win64" / "win32"

constexpr quint16 kMachineX64 = 0x8664;
constexpr quint16 kMachineX86 = 0x014c;
quint16 machineFor(Arch arch);

// What a release asset is called; the plugin builds every download URL from this and the validated
// version, never from text in the manifest. TSMedia-2.2.0-win64.update / TSMedia-2.2.0-helper-win64.update
QString assetName(FileKind kind, Arch arch, const Version& version);
// File name on disk: tsmedia_win64.dll / tsmedia_update_helper.exe
QString targetFileName(FileKind kind, Arch arch);

struct UpdateFile {
    FileKind   kind = FileKind::Plugin;
    Arch       arch = Arch::Win64;
    QString    key;     // the manifest key, e.g. "plugins/tsmedia_win64.dll"
    qint64     size = 0;
    QByteArray sha256;  // 32 raw bytes
    quint16    machine = 0;
};

struct PackageInfo {
    bool       present = false;
    QString    name; // TSMedia-<version>.ts3_plugin
    qint64     size = 0;
    QByteArray sha256;
};

struct Manifest {
    Version             version;
    QDate               published;      // invalid if missing
    Version             minFromVersion; // invalid if missing
    PackageInfo         package;
    QVector<UpdateFile> files;
    bool                needsNewerUpdater = false; // an unknown "files" entry is marked "required"
    QStringList         notes;                     // at most 5, sanitised, plain text
    QVector<int>        revokeKeys;                // as listed; see honouredRevocations()
    int                 signedBy = 0;

    const UpdateFile* file(FileKind kind, Arch arch) const;
};

enum class ManifestError {
    None,
    TooLarge,
    NotJson,
    UnsupportedFormat, // a format this plugin doesn't know (a newer one)
    BadEncoding,       // payload or sig isn't strict base64, or sig isn't 64 bytes
    UnknownKey,        // signed with a key this plugin doesn't have
    RevokedKey,
    RecoveryWithoutRevocation,
    BadSignature,
    BadPayload,
};
QString errorCode(ManifestError error); // for logs and state.ini: "badSignature", ...

struct SignedManifest {
    int        format = 0;
    int        keyId  = 0;
    QByteArray payload;
    QByteArray signature;
};

struct TrustedKey {
    int                  id       = 0;
    bool                 recovery = false;
    const unsigned char* xy       = nullptr; // 64 bytes
};

// The release keys, plus the test key in TSMEDIA_TESTHOOKS builds.
QVector<TrustedKey> trustedKeys();

constexpr int kMaxOuterBytes   = 64 * 1024;
constexpr int kMaxPayloadBytes = 48 * 1024;
constexpr int kMaxNotes        = 5;
constexpr int kMaxNoteLength   = 160;
constexpr qint64 kMaxFileSize  = 32 * 1024 * 1024;

ManifestError parseSigned(const QByteArray& bytes, SignedManifest* out);
ManifestError verifySigned(const SignedManifest& manifest, const QVector<TrustedKey>& keys, const QSet<int>& revoked);
// Only for payload bytes whose signature has been checked. *detail names the first bad field.
ManifestError parsePayload(const QByteArray& payload, Manifest* out, QString* detail = nullptr);

// parseSigned + verifySigned + parsePayload, plus the recovery rule: a manifest signed with a
// recovery key must revoke every daily key that isn't revoked yet.
ManifestError readManifest(const QByteArray& bytes, const QVector<TrustedKey>& keys, const QSet<int>& revoked, Manifest* out,
                           QString* detail = nullptr);

// The ids of manifest.revokeKeys that are honoured: never the signing key itself, and a recovery key
// only when a recovery key signed. The caller stores them for good (state.ini [trust]).
QSet<int> honouredRevocations(const Manifest& manifest, const QVector<TrustedKey>& keys);

// Control and bidi characters removed, whitespace collapsed, at most kMaxNoteLength characters.
QString sanitizeNote(const QString& text);

enum class Offer {
    No,                 // not newer, or no build for this architecture
    Yes,
    Skipped,            // the user skipped this version (a manual check still shows it)
    NeedsManualInstall, // newer, but this plugin can't install it itself
};
// effectiveInstalled: the version that runs after the next restart (state.ini "to" while an update
// is applied but not started yet, otherwise this build's version).
Offer offerFor(const Manifest& manifest, const Version& effectiveInstalled, const Version& skipped, Arch arch);

} // namespace upd
