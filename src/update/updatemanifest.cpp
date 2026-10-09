#include "updatemanifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <cmath>

#include "crypto.h"
#include "updatekeys.h"

namespace upd {

namespace {

// Strings that end up in Qt state are built with fromLatin1 (see settings.cpp); these are only
// compared against, but stay consistent.
QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

bool readInteger(const QJsonValue& value, qint64 min, qint64 max, qint64* out)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < static_cast<double>(min) || d > static_cast<double>(max))
        return false;
    *out = static_cast<qint64>(d);
    return true;
}

bool strictBase64(const QJsonValue& value, int maxDecoded, QByteArray* out)
{
    if (!value.isString())
        return false;
    const QByteArray text = value.toString().toLatin1();
    if (text.isEmpty() || text.size() > (maxDecoded / 3 + 1) * 4)
        return false;
    for (char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        if (!ok)
            return false;
    }
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(text, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok || decoded.decoded.size() > maxDecoded)
        return false;
    // Canonical only: one string per byte sequence.
    if (decoded.decoded.toBase64() != text)
        return false;
    *out = decoded.decoded;
    return true;
}

bool lowerHex64(const QJsonValue& value, QByteArray* raw)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.size() != 64)
        return false;
    for (QChar c : text) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    *raw = QByteArray::fromHex(text.toLatin1());
    return raw->size() == 32;
}

bool isStrippedChar(uint cp)
{
    if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) // C0 and C1 controls (tabs and newlines become spaces first)
        return true;
    switch (cp) {
    case 0x061c: // Arabic letter mark
    case 0x200b: // zero-width space, non-joiner, joiner
    case 0x200c:
    case 0x200d:
    case 0x200e: // LRM, RLM
    case 0x200f:
    case 0x2028: // line and paragraph separators
    case 0x2029:
    case 0xfeff:
        return true;
    default:
        break;
    }
    return (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) || (cp >= 0xfff9 && cp <= 0xfffb);
}

struct KnownFile {
    const char* key;
    FileKind    kind;
    Arch        arch;
};

constexpr KnownFile kKnownFiles[] = {
    {"plugins/tsmedia_win64.dll", FileKind::Plugin, Arch::Win64},
    {"plugins/tsmedia_win32.dll", FileKind::Plugin, Arch::Win32},
    {"helper/tsmedia_update_helper_win64.exe", FileKind::Helper, Arch::Win64},
    {"helper/tsmedia_update_helper_win32.exe", FileKind::Helper, Arch::Win32},
};

const TrustedKey* findKey(const QVector<TrustedKey>& keys, int id)
{
    for (const TrustedKey& key : keys) {
        if (key.id == id)
            return &key;
    }
    return nullptr;
}

ManifestError fail(QString* detail, const char* what)
{
    if (detail)
        *detail = latin(what);
    return ManifestError::BadPayload;
}

} // namespace

// ---- Version -----------------------------------------------------------------------------------

QString Version::toString() const
{
    if (!isValid())
        return QString();
    return QString::number(major) + QLatin1Char('.') + QString::number(minor) + QLatin1Char('.') + QString::number(patch);
}

Version Version::parse(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return {};
    int numbers[3] = {};
    for (int i = 0; i < 3; ++i) {
        const QString& part = parts.at(i);
        if (part.isEmpty() || part.size() > 3 || (part.size() > 1 && part.at(0) == QLatin1Char('0')))
            return {};
        for (QChar c : part) {
            if (c.unicode() < '0' || c.unicode() > '9')
                return {};
        }
        numbers[i] = part.toInt();
    }
    Version v;
    v.major = numbers[0];
    v.minor = numbers[1];
    v.patch = numbers[2];
    return v;
}

// ---- names -------------------------------------------------------------------------------------

Arch runningArch()
{
    return sizeof(void*) == 8 ? Arch::Win64 : Arch::Win32;
}

QString archName(Arch arch)
{
    return latin(arch == Arch::Win64 ? "win64" : "win32");
}

quint16 machineFor(Arch arch)
{
    return arch == Arch::Win64 ? kMachineX64 : kMachineX86;
}

QString assetName(FileKind kind, Arch arch, const Version& version)
{
    return latin("TSMedia-") + version.toString() + latin(kind == FileKind::Helper ? "-helper-" : "-") + archName(arch) + latin(".update");
}

QString targetFileName(FileKind kind, Arch arch)
{
    if (kind == FileKind::Helper)
        return latin("tsmedia_update_helper.exe");
    return latin("tsmedia_") + archName(arch) + latin(".dll");
}

const UpdateFile* Manifest::file(FileKind kind, Arch arch) const
{
    for (const UpdateFile& f : files) {
        if (f.kind == kind && f.arch == arch)
            return &f;
    }
    return nullptr;
}

QString errorCode(ManifestError error)
{
    switch (error) {
    case ManifestError::None:
        return latin("ok");
    case ManifestError::TooLarge:
        return latin("tooLarge");
    case ManifestError::NotJson:
        return latin("notJson");
    case ManifestError::UnsupportedFormat:
        return latin("unsupportedFormat");
    case ManifestError::BadEncoding:
        return latin("badEncoding");
    case ManifestError::UnknownKey:
        return latin("unknownKey");
    case ManifestError::RevokedKey:
        return latin("revokedKey");
    case ManifestError::RecoveryWithoutRevocation:
        return latin("recoveryWithoutRevocation");
    case ManifestError::BadSignature:
        return latin("badSignature");
    case ManifestError::BadPayload:
        return latin("badPayload");
    }
    return latin("unknown");
}

QVector<TrustedKey> trustedKeys()
{
    QVector<TrustedKey> keys;
    for (const UpdateKey& key : kReleaseKeys)
        keys.append(TrustedKey{key.id, key.recovery, key.xy});
#ifdef TSMEDIA_TESTHOOKS
    keys.append(TrustedKey{kTestKey.id, kTestKey.recovery, kTestKey.xy});
#endif
    return keys;
}

// ---- outer file and signature ------------------------------------------------------------------

ManifestError parseSigned(const QByteArray& bytes, SignedManifest* out)
{
    *out = SignedManifest();
    if (bytes.size() > kMaxOuterBytes)
        return ManifestError::TooLarge;
    QJsonParseError     error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return ManifestError::NotJson;
    const QJsonObject outer = doc.object();

    qint64 format = 0;
    if (!readInteger(outer.value(latin("format")), 0, 1000000, &format))
        return ManifestError::NotJson;
    if (format != 1)
        return ManifestError::UnsupportedFormat;
    qint64 key = 0;
    if (!readInteger(outer.value(latin("key")), 1, 255, &key))
        return ManifestError::UnknownKey;
    if (!strictBase64(outer.value(latin("payload")), kMaxPayloadBytes, &out->payload))
        return ManifestError::BadEncoding;
    if (!strictBase64(outer.value(latin("sig")), 64, &out->signature) || out->signature.size() != 64)
        return ManifestError::BadEncoding;
    out->format = static_cast<int>(format);
    out->keyId  = static_cast<int>(key);
    return ManifestError::None;
}

ManifestError verifySigned(const SignedManifest& manifest, const QVector<TrustedKey>& keys, const QSet<int>& revoked)
{
    const TrustedKey* key = findKey(keys, manifest.keyId);
    if (!key || !key->xy)
        return ManifestError::UnknownKey;
    if (revoked.contains(manifest.keyId))
        return ManifestError::RevokedKey;
    unsigned char xy[64];
    for (int i = 0; i < 64; ++i)
        xy[i] = key->xy[i];
    return crypto::ecdsaP256Verify(xy, manifest.payload, manifest.signature) ? ManifestError::None : ManifestError::BadSignature;
}

// ---- payload -----------------------------------------------------------------------------------

QString sanitizeNote(const QString& text)
{
    QString result;
    result.reserve(qMin(text.size(), kMaxNoteLength * 2));
    bool                  space = false;
    const QVector<uint>   cps   = text.toUcs4();
    int                   count = 0;
    bool                  cut   = false;
    for (uint cp : cps) {
        if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029 || QChar::isSpace(cp)) {
            space = !result.isEmpty();
            continue;
        }
        if (isStrippedChar(cp) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            continue;
        if (space) {
            if (count + 1 >= kMaxNoteLength) {
                cut = true;
                break;
            }
            result += QLatin1Char(' ');
            ++count;
            space = false;
        }
        if (count + 1 > kMaxNoteLength) {
            cut = true;
            break;
        }
        if (QChar::requiresSurrogates(cp)) {
            result += QChar(QChar::highSurrogate(cp));
            result += QChar(QChar::lowSurrogate(cp));
        } else {
            result += QChar(static_cast<ushort>(cp));
        }
        ++count;
    }
    if (cut) {
        // Room for the ellipsis: drop the last character (both halves of a surrogate pair).
        if (!result.isEmpty() && result.at(result.size() - 1).isLowSurrogate())
            result.chop(2);
        else if (!result.isEmpty())
            result.chop(1);
        while (result.endsWith(QLatin1Char(' ')))
            result.chop(1);
        result += QChar(0x2026);
    }
    return result;
}

ManifestError parsePayload(const QByteArray& payload, Manifest* out, QString* detail)
{
    *out = Manifest();
    if (detail)
        detail->clear();
    if (payload.size() > kMaxPayloadBytes)
        return ManifestError::TooLarge;
    QJsonParseError     error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return fail(detail, "payload is not a JSON object");
    const QJsonObject p = doc.object();

    qint64 format = 0;
    if (!readInteger(p.value(latin("format")), 0, 1000000, &format) || format != 1)
        return fail(detail, "format");
    if (p.value(latin("product")).toString() != latin("tsmedia"))
        return fail(detail, "product");

    Manifest m;
    m.version = Version::parse(p.value(latin("version")).toString());
    if (!m.version.isValid())
        return fail(detail, "version");
    if (p.value(latin("tag")).toString() != latin("v") + m.version.toString())
        return fail(detail, "tag");

    // Optional, for display only.
    const QJsonValue published = p.value(latin("published"));
    if (published.isString())
        m.published = QDate::fromString(published.toString(), Qt::ISODate);

    const QJsonValue minFrom = p.value(latin("minFromVersion"));
    if (!minFrom.isUndefined() && !minFrom.isNull()) {
        m.minFromVersion = Version::parse(minFrom.toString());
        if (!m.minFromVersion.isValid())
            return fail(detail, "minFromVersion");
    }

    const QJsonValue package = p.value(latin("package"));
    if (!package.isUndefined() && !package.isNull()) {
        if (!package.isObject())
            return fail(detail, "package");
        const QJsonObject pkg = package.toObject();
        m.package.present     = true;
        m.package.name        = pkg.value(latin("name")).toString();
        if (m.package.name != latin("TSMedia-") + m.version.toString() + latin(".ts3_plugin"))
            return fail(detail, "package.name");
        if (!readInteger(pkg.value(latin("size")), 1, kMaxFileSize, &m.package.size))
            return fail(detail, "package.size");
        if (!lowerHex64(pkg.value(latin("sha256")), &m.package.sha256))
            return fail(detail, "package.sha256");
    }

    const QJsonValue filesValue = p.value(latin("files"));
    if (!filesValue.isObject())
        return fail(detail, "files");
    const QJsonObject files = filesValue.toObject();
    if (files.size() > 32)
        return fail(detail, "too many files");
    for (auto it = files.begin(); it != files.end(); ++it) {
        const KnownFile* known = nullptr;
        for (const KnownFile& k : kKnownFiles) {
            if (it.key() == latin(k.key))
                known = &k;
        }
        if (!known) {
            // A file a later version needs. Ignored unless the release can't work without it.
            if (it.value().isObject() && it.value().toObject().value(latin("required")).toBool(false))
                m.needsNewerUpdater = true;
            continue;
        }
        if (!it.value().isObject())
            return fail(detail, "files entry");
        const QJsonObject entry = it.value().toObject();
        UpdateFile        f;
        f.kind = known->kind;
        f.arch = known->arch;
        f.key  = it.key();
        if (!readInteger(entry.value(latin("size")), 1, kMaxFileSize, &f.size))
            return fail(detail, "files size");
        if (!lowerHex64(entry.value(latin("sha256")), &f.sha256))
            return fail(detail, "files sha256");
        // The machine must match the file name: a win64 name with x86 code is refused.
        const QString machine = entry.value(latin("machine")).toString();
        if (machine != latin(f.arch == Arch::Win64 ? "x64" : "x86"))
            return fail(detail, "files machine");
        f.machine = machineFor(f.arch);
        // Informational; the download URL is always built from assetName().
        const QJsonValue asset = entry.value(latin("asset"));
        if (!asset.isUndefined() && asset.toString() != assetName(f.kind, f.arch, m.version))
            return fail(detail, "files asset");
        m.files.append(f);
    }
    if (!m.file(FileKind::Plugin, Arch::Win64) && !m.file(FileKind::Plugin, Arch::Win32) && !m.needsNewerUpdater)
        return fail(detail, "no plugin file");

    const QJsonValue notes = p.value(latin("notes"));
    if (notes.isArray()) {
        for (const QJsonValue& note : notes.toArray()) {
            if (m.notes.size() >= kMaxNotes)
                break;
            if (!note.isString())
                continue;
            const QString clean = sanitizeNote(note.toString());
            if (!clean.isEmpty())
                m.notes.append(clean);
        }
    }

    const QJsonValue revoke = p.value(latin("revokeKeys"));
    if (!revoke.isUndefined() && !revoke.isNull()) {
        if (!revoke.isArray() || revoke.toArray().size() > 16)
            return fail(detail, "revokeKeys");
        for (const QJsonValue& id : revoke.toArray()) {
            qint64 value = 0;
            if (!readInteger(id, 1, 255, &value))
                return fail(detail, "revokeKeys");
            if (!m.revokeKeys.contains(static_cast<int>(value)))
                m.revokeKeys.append(static_cast<int>(value));
        }
    }

    *out = m;
    return ManifestError::None;
}

QSet<int> honouredRevocations(const Manifest& manifest, const QVector<TrustedKey>& keys)
{
    const TrustedKey* signer = findKey(keys, manifest.signedBy);
    QSet<int>         result;
    for (int id : manifest.revokeKeys) {
        if (id == manifest.signedBy)
            continue; // a key never revokes itself
        const TrustedKey* target = findKey(keys, id);
        // A daily key can't retire the recovery key: a stolen daily key could otherwise block recovery.
        if (target && target->recovery && !(signer && signer->recovery))
            continue;
        result.insert(id);
    }
    return result;
}

ManifestError readManifest(const QByteArray& bytes, const QVector<TrustedKey>& keys, const QSet<int>& revoked, Manifest* out, QString* detail)
{
    *out = Manifest();
    if (detail)
        detail->clear();
    SignedManifest signedManifest;
    ManifestError  error = parseSigned(bytes, &signedManifest);
    if (error != ManifestError::None)
        return error;
    error = verifySigned(signedManifest, keys, revoked);
    if (error != ManifestError::None)
        return error;
    Manifest m;
    error = parsePayload(signedManifest.payload, &m, detail);
    if (error != ManifestError::None)
        return error;
    m.signedBy = signedManifest.keyId;

    const TrustedKey* signer = findKey(keys, m.signedBy);
    if (signer && signer->recovery) {
        // Using the recovery key means a daily key is lost or leaked: it must be retired at once.
        for (const TrustedKey& key : keys) {
            if (!key.recovery && key.id != m.signedBy && !revoked.contains(key.id) && !m.revokeKeys.contains(key.id)) {
#ifdef TSMEDIA_TESTHOOKS
                if (key.id == kTestKey.id)
                    continue;
#endif
                return ManifestError::RecoveryWithoutRevocation;
            }
        }
    }
    *out = m;
    return ManifestError::None;
}

Offer offerFor(const Manifest& manifest, const Version& effectiveInstalled, const Version& skipped, Arch arch)
{
    if (!manifest.version.isValid() || !effectiveInstalled.isValid() || !(manifest.version > effectiveInstalled))
        return Offer::No;
    if (manifest.needsNewerUpdater || (manifest.minFromVersion.isValid() && effectiveInstalled < manifest.minFromVersion))
        return Offer::NeedsManualInstall;
    if (!manifest.file(FileKind::Plugin, arch))
        return Offer::No; // this release has no build for this TeamSpeak (e.g. no 32-bit DLL)
    if (skipped.isValid() && skipped == manifest.version)
        return Offer::Skipped;
    return Offer::Yes;
}

} // namespace upd
