// Unit tests for the updater (docs/UPDATES.md): SHA-256 and ECDSA through CNG, the signed manifest
// (including hostile inputs), versions, the URL allowlist and schedule, the PE checks, and the
// installer against a temporary plugins folder (swap, undo, rename of a loaded DLL, self-rollback,
// startup reconcile, cleanup). Runs inside tsmedia_tests (tests/testmain.h).
//
// The signed vectors in tests/data/update were made with the test key 99, whose private key lives
// outside the repository (C:/dev/tsmedia-devtools/signing). This test is built without
// TSMEDIA_TESTHOOKS, so trustedKeys() must not accept them.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

#include "crypto.h"
#include "update/pecheck.h"
#include "update/updatefiles.h"
#include "update/updateinstaller.h"
#include "update/updatekeys.h"
#include "update/updatemanifest.h"
#include "update/updatepolicy.h"
#include "update/updatesettings.h"
#include "testmain.h"

using namespace upd;

namespace {

QByteArray readData(const char* name)
{
    QFile file(QStringLiteral(TSMEDIA_TEST_DATA_DIR "/") + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing test vector %s", name);
    return file.readAll();
}

QVector<TrustedKey> testKeys(bool testKeyIsRecovery = false)
{
    QVector<TrustedKey> keys;
    for (const UpdateKey& key : kReleaseKeys)
        keys.append(TrustedKey{key.id, key.recovery, key.xy});
    keys.append(TrustedKey{kTestKey.id, testKeyIsRecovery, kTestKey.xy});
    return keys;
}

QString hex64(char c)
{
    return QString(64, QLatin1Char(c));
}

// A valid payload as an object, to be broken one field at a time.
QJsonObject basePayload()
{
    QJsonObject win64{{QStringLiteral("size"), 1000}, {QStringLiteral("sha256"), hex64('a')}, {QStringLiteral("machine"), QStringLiteral("x64")}};
    QJsonObject win32{{QStringLiteral("size"), 900}, {QStringLiteral("sha256"), hex64('b')}, {QStringLiteral("machine"), QStringLiteral("x86")}};
    QJsonObject files{{QStringLiteral("plugins/tsmedia_win64.dll"), win64}, {QStringLiteral("plugins/tsmedia_win32.dll"), win32}};
    return QJsonObject{{QStringLiteral("format"), 1},
                       {QStringLiteral("product"), QStringLiteral("tsmedia")},
                       {QStringLiteral("version"), QStringLiteral("2.2.0")},
                       {QStringLiteral("tag"), QStringLiteral("v2.2.0")},
                       {QStringLiteral("files"), files}};
}

QByteArray toJson(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

ManifestError parse(const QJsonObject& payload, Manifest* out = nullptr)
{
    Manifest m;
    const ManifestError e = parsePayload(toJson(payload), &m);
    if (out)
        *out = m;
    return e;
}

QJsonObject withFile(QJsonObject payload, const QString& key, const QJsonValue& value)
{
    QJsonObject files = payload.value(QStringLiteral("files")).toObject();
    if (value.isUndefined())
        files.remove(key);
    else
        files.insert(key, value);
    payload.insert(QStringLiteral("files"), files);
    return payload;
}

QJsonObject withWin64Field(const QJsonObject& payload, const QString& field, const QJsonValue& value)
{
    QJsonObject entry = payload.value(QStringLiteral("files")).toObject().value(QStringLiteral("plugins/tsmedia_win64.dll")).toObject();
    entry.insert(field, value);
    return withFile(payload, QStringLiteral("plugins/tsmedia_win64.dll"), entry);
}

QString systemDll(bool x86)
{
    wchar_t dir[MAX_PATH] = {};
    if (x86)
        GetSystemWow64DirectoryW(dir, MAX_PATH);
    else
        GetSystemDirectoryW(dir, MAX_PATH);
    return QDir::fromNativeSeparators(QString::fromWCharArray(dir)) + QStringLiteral("/version.dll");
}

// A real PE file of the right CPU with some bytes appended (an overlay), so "old" and "new" differ.
bool writeFakeDll(const QString& path, bool x86, const QByteArray& tag)
{
    QFile source(systemDll(x86));
    QFile target(path);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return target.write(source.readAll()) > 0 && target.write(tag) == tag.size();
}

QByteArray sha(const QString& path)
{
    return fs::sha256(path);
}

struct Sandbox {
    QTemporaryDir dir;
    Layout        layout;

    Sandbox()
    {
        layout.pluginsDir = dir.path() + QStringLiteral("/config/plugins");
        layout.updateDir  = dir.path() + QStringLiteral("/config/plugins/tsmedia/update");
        layout.configDir  = dir.path() + QStringLiteral("/config");
        layout.arch       = Arch::Win64;
        fs::ensureDir(layout.pluginsDir);
        fs::ensureDir(layout.updateDir);
    }

    QString plugin(const char* name) const { return layout.pluginsDir + QLatin1Char('/') + QLatin1String(name); }

    // Old version installed, new version staged.
    bool setUp(const Version& to, QVector<StagedFile>* staged, bool withWin32 = true)
    {
        if (!writeFakeDll(plugin("tsmedia_win64.dll"), false, "old64") || !writeFakeDll(plugin("tsmedia_win32.dll"), true, "old32"))
            return false;
        const QString stagingDir = layout.stagingDir(to);
        fs::ensureDir(stagingDir);
        staged->clear();
        const struct {
            Arch arch;
            bool x86;
            const char* tag;
        } items[] = {{Arch::Win64, false, "new64"}, {Arch::Win32, true, "new32"}};
        for (const auto& item : items) {
            if (item.arch == Arch::Win32 && !withWin32)
                continue;
            StagedFile f;
            f.arch = item.arch;
            f.path = stagingDir + QLatin1Char('/') + targetFileName(FileKind::Plugin, item.arch) + QStringLiteral(".new");
            if (!writeFakeDll(f.path, item.x86, item.tag))
                return false;
            f.sha256 = sha(f.path);
            f.size   = fs::fileSize(f.path);
            staged->append(f);
        }
        return true;
    }
};

} // namespace

class TestUpdater : public QObject
{
    Q_OBJECT

  private slots:
    // ---- crypto ---------------------------------------------------------------------------------
    void sha256Vectors()
    {
        QCOMPARE(crypto::Sha256::hash(QByteArray()).toHex(), QByteArray("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        QCOMPARE(crypto::Sha256::hash(QByteArray("abc")).toHex(), QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QByteArray big(1024 * 1024, '\0');
        for (int i = 0; i < big.size(); ++i)
            big[i] = static_cast<char>((i * 31 + 7) & 0xff);
        crypto::Sha256 streamed;
        for (int pos = 0, step = 1; pos < big.size(); pos += step, step = step * 3 % 65521 + 1)
            streamed.add(big.constData() + pos, static_cast<size_t>(qMin(step, big.size() - pos)));
        QCOMPARE(streamed.finish(), QCryptographicHash::hash(big, QCryptographicHash::Sha256));
        QCOMPARE(streamed.finish(), QByteArray()); // spent
    }

    void ecdsaSignature()
    {
        SignedManifest s;
        QCOMPARE(parseSigned(readData("valid.json"), &s), ManifestError::None);
        QCOMPARE(s.keyId, 99);
        QVERIFY(crypto::ecdsaP256Verify(kTestKey.xy, s.payload, s.signature));

        QByteArray payload = s.payload;
        payload[10]        = static_cast<char>(payload[10] ^ 0x01);
        QVERIFY(!crypto::ecdsaP256Verify(kTestKey.xy, payload, s.signature));
        QByteArray sig = s.signature;
        sig[5]         = static_cast<char>(sig[5] ^ 0x80);
        QVERIFY(!crypto::ecdsaP256Verify(kTestKey.xy, s.payload, sig));
        QVERIFY(!crypto::ecdsaP256Verify(kTestKey.xy, s.payload, s.signature.left(63)));
        QVERIFY(!crypto::ecdsaP256Verify(kTestKey.xy, s.payload, QByteArray(64, '\0')));
        QVERIFY(!crypto::ecdsaP256Verify(kReleaseKeys[0].xy, s.payload, s.signature)); // another key
    }

    void testKeyNotTrustedWithoutTesthooks()
    {
        Manifest m;
        for (const TrustedKey& key : trustedKeys())
            QVERIFY(key.id != 99);
        QCOMPARE(readManifest(readData("valid.json"), trustedKeys(), {}, &m), ManifestError::UnknownKey);
    }

    // ---- versions -------------------------------------------------------------------------------
    void versions()
    {
        QVERIFY(Version::parse(QStringLiteral("2.2.0")) < Version::parse(QStringLiteral("2.2.1")));
        QVERIFY(Version::parse(QStringLiteral("2.2.1")) < Version::parse(QStringLiteral("2.10.0")));
        QVERIFY(Version::parse(QStringLiteral("0.0.0")).isValid());
        QCOMPARE(Version::parse(QStringLiteral("999.999.999")).toString(), QStringLiteral("999.999.999"));
        for (const char* bad : {"2.2", "2.2.0-beta", "v2.2.0", "2.2.0 ", " 2.2.0", "1000.0.0", "02.1.0", "2..0", "2.2.0.0", "", "a.b.c", "2.2.+1", "\u0662.\u0662.\u0660"})
            QVERIFY2(!Version::parse(QString::fromUtf8(bad)).isValid(), bad);
    }

    // ---- manifest -------------------------------------------------------------------------------
    void readValidManifest()
    {
        Manifest m;
        QString  detail;
        QCOMPARE(readManifest(readData("valid.json"), testKeys(), {}, &m, &detail), ManifestError::None);
        QCOMPARE(m.version.toString(), QStringLiteral("9.0.0"));
        QCOMPARE(m.signedBy, 99);
        QCOMPARE(m.published, QDate(2026, 11, 2));
        QCOMPARE(m.minFromVersion.toString(), QStringLiteral("2.1.0"));
        QVERIFY(m.package.present);
        QCOMPARE(m.package.size, qint64(1203456));
        QCOMPARE(m.files.size(), 4); // data/future.bin is ignored
        QVERIFY(!m.needsNewerUpdater);
        const UpdateFile* w64 = m.file(FileKind::Plugin, Arch::Win64);
        QVERIFY(w64);
        QCOMPARE(w64->size, qint64(655360));
        QCOMPARE(w64->sha256, QByteArray(32, static_cast<char>(0xaa)));
        QCOMPARE(w64->machine, kMachineX64);
        QVERIFY(m.file(FileKind::Helper, Arch::Win32));
        QCOMPARE(m.notes.size(), 3);
        QCOMPARE(m.notes.first(), QStringLiteral("Spoilers: blur an image or video until someone clicks it"));
        QCOMPARE(assetName(FileKind::Helper, Arch::Win64, m.version), QStringLiteral("TSMedia-9.0.0-helper-win64.update"));
        QCOMPARE(assetName(FileKind::Plugin, Arch::Win32, m.version), QStringLiteral("TSMedia-9.0.0-win32.update"));
    }

    void tamperedOuter()
    {
        const QByteArray valid = readData("valid.json");
        QJsonObject      outer = QJsonDocument::fromJson(valid).object();
        Manifest         m;

        QByteArray payload = QByteArray::fromBase64(outer.value(QStringLiteral("payload")).toString().toLatin1());
        payload.replace("9.0.0", "9.0.9");
        QJsonObject changed = outer;
        changed.insert(QStringLiteral("payload"), QString::fromLatin1(payload.toBase64()));
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::BadSignature);

        QByteArray sig = QByteArray::fromBase64(outer.value(QStringLiteral("sig")).toString().toLatin1());
        sig[0]         = static_cast<char>(sig[0] ^ 1);
        changed        = outer;
        changed.insert(QStringLiteral("sig"), QString::fromLatin1(sig.toBase64()));
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::BadSignature);

        changed = outer;
        changed.insert(QStringLiteral("sig"), QString::fromLatin1(sig.left(63).toBase64()));
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::BadEncoding);

        changed = outer;
        changed.insert(QStringLiteral("key"), 7);
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::UnknownKey);
        changed.insert(QStringLiteral("key"), 1); // a real key, but it didn't sign this
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::BadSignature);

        QCOMPARE(readManifest(valid, testKeys(), {99}, &m), ManifestError::RevokedKey);

        changed = outer;
        changed.insert(QStringLiteral("format"), 2);
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::UnsupportedFormat);
        changed = outer;
        changed.insert(QStringLiteral("payload"), QStringLiteral("not base64!"));
        QCOMPARE(readManifest(toJson(changed), testKeys(), {}, &m), ManifestError::BadEncoding);
        changed = outer; // non-canonical padding bits
        QString text = outer.value(QStringLiteral("sig")).toString();
        text[text.size() - 3] = text.at(text.size() - 3) == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        changed.insert(QStringLiteral("sig"), text);
        QVERIFY(readManifest(toJson(changed), testKeys(), {}, &m) != ManifestError::None);

        QCOMPARE(readManifest(QByteArray("[1,2,3]"), testKeys(), {}, &m), ManifestError::NotJson);
        QCOMPARE(readManifest(QByteArray("{\"format\":1"), testKeys(), {}, &m), ManifestError::NotJson);
        QCOMPARE(readManifest(QByteArray(70 * 1024, ' '), testKeys(), {}, &m), ManifestError::TooLarge);
        QCOMPARE(readManifest(QByteArray(), testKeys(), {}, &m), ManifestError::NotJson);
    }

    void hostilePayloads()
    {
        QCOMPARE(parse(basePayload()), ManifestError::None);
        const QJsonObject base = basePayload();
        struct Case {
            const char* name;
            QJsonObject payload;
        };
        auto set = [&base](const char* key, const QJsonValue& value) {
            QJsonObject p = base;
            p.insert(QLatin1String(key), value);
            return p;
        };
        const QString big = QString::number(33 * 1024 * 1024);
        const QVector<Case> cases = {
            {"product", set("product", QStringLiteral("other"))},
            {"format 2", set("format", 2)},
            {"format string", set("format", QStringLiteral("1"))},
            {"version suffix", set("version", QStringLiteral("2.2.0-beta"))},
            {"version short", set("version", QStringLiteral("2.2"))},
            {"version v", set("version", QStringLiteral("v2.2.0"))},
            {"version space", set("version", QStringLiteral("2.2.0 "))},
            {"version 1000", set("version", QStringLiteral("1000.0.0"))},
            {"tag mismatch", set("tag", QStringLiteral("v2.2.1"))},
            {"tag missing", [&] { QJsonObject p = base; p.remove(QStringLiteral("tag")); return p; }()},
            {"files missing", [&] { QJsonObject p = base; p.remove(QStringLiteral("files")); return p; }()},
            {"files array", set("files", QJsonArray{1, 2})},
            {"size 0", withWin64Field(base, QStringLiteral("size"), 0)},
            {"size negative", withWin64Field(base, QStringLiteral("size"), -5)},
            {"size fraction", withWin64Field(base, QStringLiteral("size"), 10.5)},
            {"size over 32 MiB", withWin64Field(base, QStringLiteral("size"), 33 * 1024 * 1024)},
            {"size string", withWin64Field(base, QStringLiteral("size"), big)},
            {"sha uppercase", withWin64Field(base, QStringLiteral("sha256"), QString(64, QLatin1Char('A')))},
            {"sha short", withWin64Field(base, QStringLiteral("sha256"), QString(63, QLatin1Char('a')))},
            {"sha not hex", withWin64Field(base, QStringLiteral("sha256"), QString(64, QLatin1Char('g')))},
            {"machine mismatch", withWin64Field(base, QStringLiteral("machine"), QStringLiteral("x86"))},
            {"machine missing", withWin64Field(base, QStringLiteral("machine"), QJsonValue())},
            {"asset mismatch", withWin64Field(base, QStringLiteral("asset"), QStringLiteral("../../evil.update"))},
            {"no plugin left", withFile(withFile(base, QStringLiteral("plugins/tsmedia_win64.dll"), QJsonValue::Undefined),
                                        QStringLiteral("plugins/tsmedia_win32.dll"), QJsonValue::Undefined)},
            {"package other version", set("package", QJsonObject{{QStringLiteral("name"), QStringLiteral("TSMedia-2.1.0.ts3_plugin")},
                                                                 {QStringLiteral("size"), 10}, {QStringLiteral("sha256"), hex64('c')}})},
            {"package size 0", set("package", QJsonObject{{QStringLiteral("name"), QStringLiteral("TSMedia-2.2.0.ts3_plugin")},
                                                          {QStringLiteral("size"), 0}, {QStringLiteral("sha256"), hex64('c')}})},
            {"minFromVersion bad", set("minFromVersion", QStringLiteral("2.1"))},
            {"revokeKeys string", set("revokeKeys", QJsonArray{QStringLiteral("1")})},
            {"revokeKeys 0", set("revokeKeys", QJsonArray{0})},
        };
        for (const Case& c : cases)
            QVERIFY2(parse(c.payload) == ManifestError::BadPayload, c.name);

        Manifest m;
        QCOMPARE(parsePayload(QByteArray("[]"), &m), ManifestError::BadPayload);
        QCOMPARE(parsePayload(QByteArray(49 * 1024, ' '), &m), ManifestError::TooLarge);

        // Unknown files: ignored, also path tricks; "required" ones need a newer updater.
        QCOMPARE(parse(withFile(base, QStringLiteral("plugins/../x.dll"), QJsonObject{{QStringLiteral("size"), 1}}), &m), ManifestError::None);
        QCOMPARE(m.files.size(), 2);
        QCOMPARE(parse(withFile(base, QStringLiteral("plugins/evil.dll"), QJsonObject{{QStringLiteral("size"), 1}}), &m), ManifestError::None);
        QCOMPARE(m.files.size(), 2);
        QVERIFY(!m.needsNewerUpdater);
        QCOMPARE(parse(withFile(base, QStringLiteral("plugins/extra.dll"), QJsonObject{{QStringLiteral("required"), true}}), &m), ManifestError::None);
        QVERIFY(m.needsNewerUpdater);
        QCOMPARE(offerFor(m, Version::parse(QStringLiteral("2.1.1")), {}, Arch::Win64), Offer::NeedsManualInstall);

        QJsonObject tooMany = base;
        QJsonObject files   = base.value(QStringLiteral("files")).toObject();
        for (int i = 0; i < 40; ++i)
            files.insert(QStringLiteral("x/%1").arg(i), QJsonObject());
        tooMany.insert(QStringLiteral("files"), files);
        QCOMPARE(parse(tooMany), ManifestError::BadPayload);

        // Unknown top-level fields are ignored (format 1 is frozen).
        QCOMPARE(parse(set("someday", QJsonObject{{QStringLiteral("a"), 1}})), ManifestError::None);
    }

    void notes()
    {
        QJsonObject p = basePayload();
        QJsonArray  notes;
        for (int i = 0; i < 8; ++i)
            notes.append(QStringLiteral("Note %1").arg(i));
        notes.insert(1, 42); // not a string: skipped
        p.insert(QStringLiteral("notes"), notes);
        Manifest m;
        QCOMPARE(parse(p, &m), ManifestError::None);
        QCOMPARE(m.notes.size(), kMaxNotes);
        QCOMPARE(m.notes.at(1), QStringLiteral("Note 1"));

        QCOMPARE(sanitizeNote(QString::fromUtf8("  a\tb\n\nc  ")), QStringLiteral("a b c"));
        QCOMPARE(sanitizeNote(QString::fromUtf8("x\u202Egnp.exe\u202C\u200F")), QStringLiteral("xgnp.exe"));
        QCOMPARE(sanitizeNote(QString::fromUtf8("a\x01\x7f" "b\xc2\x85" "c")), QStringLiteral("ab c")); // U+0085 is a line break
        QCOMPARE(sanitizeNote(QStringLiteral("<b>bold</b> &amp;")), QStringLiteral("<b>bold</b> &amp;")); // stays literal text
        const QString longNote = sanitizeNote(QString(400, QLatin1Char('x')));
        QCOMPARE(longNote.size(), kMaxNoteLength);
        QVERIFY(longNote.endsWith(QChar(0x2026)));
        QCOMPARE(sanitizeNote(QString(kMaxNoteLength, QLatin1Char('y'))).size(), kMaxNoteLength);
        // Never cuts a surrogate pair in half.
        QString emoji;
        for (int i = 0; i < 200; ++i)
            emoji += QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString cut = sanitizeNote(emoji);
        QVERIFY(cut.size() <= kMaxNoteLength * 2);
        for (int i = 0; i + 1 < cut.size(); ++i) {
            if (cut.at(i).isHighSurrogate())
                QVERIFY(cut.at(i + 1).isLowSurrogate());
        }
        QVERIFY(!cut.at(cut.size() - 2).isHighSurrogate());
        QCOMPARE(sanitizeNote(QString::fromUtf8("\u200B\u200E")), QString());
    }

    void recoveryRules()
    {
        Manifest m;
        // Test key 99 plays the recovery key here; key 1 is the daily key.
        const QVector<TrustedKey> keys = testKeys(true);
        QCOMPARE(readManifest(readData("recovery-norevoke.json"), keys, {}, &m), ManifestError::RecoveryWithoutRevocation);
        QCOMPARE(readManifest(readData("recovery-norevoke.json"), keys, {1}, &m), ManifestError::None); // 1 is already revoked
        QCOMPARE(readManifest(readData("recovery-revoke.json"), keys, {}, &m), ManifestError::None);
        QCOMPARE(honouredRevocations(m, keys), QSet<int>({1})); // never the signer itself

        // A daily key may retire daily keys but not a recovery key (here 2).
        QCOMPARE(readManifest(readData("revoke-mixed.json"), testKeys(false), {}, &m), ManifestError::None);
        QCOMPARE(honouredRevocations(m, testKeys(false)), QSet<int>({5}));
    }

    void offers()
    {
        Manifest m;
        QCOMPARE(parse(basePayload(), &m), ManifestError::None);
        const Version v210 = Version::parse(QStringLiteral("2.1.0"));
        const Version v220 = Version::parse(QStringLiteral("2.2.0"));
        QCOMPARE(offerFor(m, v210, {}, Arch::Win64), Offer::Yes);
        QCOMPARE(offerFor(m, v220, {}, Arch::Win64), Offer::No);
        QCOMPARE(offerFor(m, Version::parse(QStringLiteral("2.3.0")), {}, Arch::Win64), Offer::No); // never a downgrade
        QCOMPARE(offerFor(m, v210, v220, Arch::Win64), Offer::Skipped);
        QCOMPARE(offerFor(m, v210, Version::parse(QStringLiteral("2.1.9")), Arch::Win64), Offer::Yes);

        QJsonObject p = basePayload();
        p.insert(QStringLiteral("minFromVersion"), QStringLiteral("2.1.5"));
        QCOMPARE(parse(p, &m), ManifestError::None);
        QCOMPARE(offerFor(m, v210, {}, Arch::Win64), Offer::NeedsManualInstall);
        QCOMPARE(offerFor(m, Version::parse(QStringLiteral("2.1.5")), {}, Arch::Win64), Offer::Yes);

        QCOMPARE(parse(withFile(basePayload(), QStringLiteral("plugins/tsmedia_win32.dll"), QJsonValue::Undefined), &m), ManifestError::None);
        QCOMPARE(offerFor(m, v210, {}, Arch::Win32), Offer::No); // no build for a 32-bit TeamSpeak
        QCOMPARE(offerFor(m, v210, {}, Arch::Win64), Offer::Yes);
    }

    // ---- policy ---------------------------------------------------------------------------------
    void urlAllowlist()
    {
        for (const char* ok : {"https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest/download/tsmedia-update.json",
                               "https://release-assets.githubusercontent.com/github-production-release-asset/1?sp=r&sig=x",
                               "https://objects.githubusercontent.com/x", "https://GitHub.com/x", "https://github.com:443/x"})
            QVERIFY2(isAllowedUrl(QUrl(QString::fromLatin1(ok))), ok);
        for (const char* bad : {"http://github.com/x", "https://github.com:8443/x", "https://user@github.com/x", "https://u:p@github.com/x",
                                "https://github.com.evil.com/x", "https://evilgithubusercontent.com/x", "https://githubusercontent.com.evil/x",
                                "https://githubusercontent.com/x", "https://140.82.112.3/x", "https://[::1]/x", "ftp://github.com/x",
                                "https://api.github.com/x", "https://a..githubusercontent.com/x", "https://-a.githubusercontent.com/x", "/relative",
                                "https://github.com./x", "file:///C:/x"})
            QVERIFY2(!isAllowedUrl(QUrl(QString::fromLatin1(bad))), bad);
        QCOMPARE(manifestUrl().toString(), QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest/download/tsmedia-update.json"));
        QCOMPARE(assetUrl(QStringLiteral("v2.2.0"), QStringLiteral("TSMedia-2.2.0-win64.update")).toString(),
                 QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat/releases/download/v2.2.0/TSMedia-2.2.0-win64.update"));
        QVERIFY(isAllowedUrl(manifestUrl()));
    }

    void schedule()
    {
        const QDateTime now(QDate(2026, 11, 2), QTime(10, 0), Qt::UTC);
        QCOMPARE(initialDelayMs(0), 3 * 60 * 1000);
        QVERIFY(initialDelayMs(0xffffffffu) <= 8 * 60 * 1000);
        for (quint32 r : {0u, 1u, 12345u, 0xffffffffu}) {
            const qint64 d = initialDelayMs(r);
            QVERIFY(d >= 3 * 60 * 1000 && d <= 8 * 60 * 1000);
            const qint64 s = now.secsTo(nextAfterSuccess(now, r));
            QVERIFY(s >= 24 * 3600 && s <= 27 * 3600);
        }
        const int hours[] = {1, 3, 6, 12, 24, 24, 24};
        for (int i = 0; i < 7; ++i)
            QCOMPARE(now.secsTo(nextAfterFailure(now, i + 1)), qint64(hours[i]) * 3600);
        QCOMPARE(now.secsTo(nextAfterFailure(now, 1, 5 * 3600)), qint64(5 * 3600)); // Retry-After pushes it later
        QCOMPARE(now.secsTo(nextAfterFailure(now, 1, 999999)), kMaxRetryAfterSec);
        QCOMPARE(sanitizeNext(now.addDays(30), now), now.addSecs(24 * 3600));
        QCOMPARE(sanitizeNext(now.addSecs(3600), now), now.addSecs(3600));
        QCOMPARE(sanitizeNext(QDateTime(), now), now);
        QVERIFY(shouldAskConsent(CheckSetting::NotAsked, 0));
        QVERIFY(shouldAskConsent(CheckSetting::NotAsked, 1));
        QVERIFY(!shouldAskConsent(CheckSetting::NotAsked, 2));
        QVERIFY(!shouldAskConsent(CheckSetting::On, 0));
        QVERIFY(!shouldAskConsent(CheckSetting::Off, 0));
        QCOMPARE(downloadDeadlineMs(1000), qint64(120000));
        QCOMPARE(downloadDeadlineMs(32 * 1024 * 1024), qint64(32 * 1024 * 1024 / 16));
    }

    void relaunch()
    {
        const QStringList args = {QStringLiteral("ts3server://host?password=secret"), QStringLiteral("-NoSingleInstance"), QStringLiteral("C:\\x"),
                                  QStringLiteral("-console"), QStringLiteral("-nosingleinstance"), QStringLiteral("-config=x"), QStringLiteral("-silentstart")};
        QCOMPARE(relaunchFlags(args), QStringList({QStringLiteral("-nosingleinstance"), QStringLiteral("-console"), QStringLiteral("-silentstart")}));
        QVERIFY(relaunchFlags({}).isEmpty());
    }

    // ---- PE checks ------------------------------------------------------------------------------
    void peInfo()
    {
        QByteArray self;
        QVERIFY(fs::readAll(QCoreApplication::applicationFilePath(), &self, 64 * 1024 * 1024));
        pe::ImageInfo info;
        QVERIFY(pe::readInfo(self, &info));
        QCOMPARE(info.machine, kMachineX64);
        QVERIFY(info.is64);
        QVERIFY(!info.isDll);

        QByteArray x86;
        QVERIFY(fs::readAll(systemDll(true), &x86, 64 * 1024 * 1024));
        QVERIFY(pe::readInfo(x86, &info));
        QCOMPARE(info.machine, kMachineX86);
        QVERIFY(info.isDll);
        QVERIFY(!info.is64);

        QString why;
        QVERIFY(!pe::readInfo(QByteArray("MZ"), &info, &why));
        QVERIFY(!pe::readInfo(QByteArray(4096, 'x'), &info));
        QVERIFY(!pe::readInfo(self.left(0x90), &info));
        QByteArray badOffset = self;
        badOffset[0x3c]      = static_cast<char>(0xff);
        badOffset[0x3d]      = static_cast<char>(0xff);
        badOffset[0x3e]      = static_cast<char>(0xff);
        badOffset[0x3f]      = static_cast<char>(0x7f);
        QVERIFY(!pe::readInfo(badOffset, &info));
        QVector<pe::ImportedModule> imports;
        pe::readImports(self.left(self.size() / 3), &imports); // must not crash on a truncated image
    }

    void peImports()
    {
        HMODULE module = GetModuleHandleW(L"Qt5Core.dll");
        QVERIFY(module);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        QByteArray image;
        QVERIFY(fs::readAll(QString::fromWCharArray(path), &image, 64 * 1024 * 1024));
        QVector<pe::ImportedModule> imports;
        QVERIFY(pe::readImports(image, &imports));
        bool kernel = false;
        for (const pe::ImportedModule& m : imports) {
            if (m.name.compare(QStringLiteral("KERNEL32.dll"), Qt::CaseInsensitive) == 0) {
                kernel = true;
                QVERIFY(m.functions.contains(QStringLiteral("GetProcAddress")) || m.functions.size() > 10);
            }
        }
        QVERIFY(kernel);
        QCOMPARE(pe::unresolvedImports(imports), QStringList());
        QCOMPARE(pe::fileVersion(QString::fromWCharArray(path)), QStringLiteral("5.15.2"));

        // The same image importing a function that doesn't exist: "CloseHandle" -> "CloseHandlX".
        const int at = image.indexOf(QByteArray("CloseHandle\0", 12));
        QVERIFY(at > 0);
        QByteArray broken = image;
        broken[at + 10]   = 'X';
        QVERIFY(pe::readImports(broken, &imports));
        QStringList missing = pe::unresolvedImports(imports);
        QVERIFY2(missing.contains(QStringLiteral("KERNEL32.dll!CloseHandlX"), Qt::CaseInsensitive), qPrintable(missing.join(QLatin1Char(','))));

        // A module nobody has.
        pe::ImportedModule ghost;
        ghost.name      = QStringLiteral("tsmedia_no_such_module_42.dll");
        ghost.functions = QStringList{QStringLiteral("f")};
        QCOMPARE(pe::unresolvedImports(QVector<pe::ImportedModule>{ghost}), QStringList{ghost.name});
        pe::ImportedModule withPath;
        withPath.name = QStringLiteral("..\\..\\Windows\\System32\\kernel32.dll");
        QCOMPARE(pe::unresolvedImports(QVector<pe::ImportedModule>{withPath}), QStringList{withPath.name}); // never loaded by path
        QCOMPARE(pe::fileVersion(QStringLiteral("C:/no/such/file.dll")), QString());
    }

    // ---- installer ------------------------------------------------------------------------------
    void applyAndRollBack()
    {
        Sandbox box;
        QVERIFY(box.dir.isValid());
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        const QByteArray old64 = sha(box.plugin("tsmedia_win64.dll"));
        const QByteArray old32 = sha(box.plugin("tsmedia_win32.dll"));

        const ApplyResult result = applyUpdate(box.layout, staged, from, to);
        QVERIFY(result.ok());
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), staged.at(0).sha256);
        QCOMPARE(sha(box.plugin("tsmedia_win32.dll")), staged.at(1).sha256);
        QCOMPARE(sha(box.layout.rollbackDir(from) + QStringLiteral("/tsmedia_win64.dll.bak")), old64);
        QCOMPARE(sha(box.layout.rollbackDir(from) + QStringLiteral("/tsmedia_win32.dll.bak")), old32);
        StateFile st(box.layout.stateFile());
        QCOMPARE(st.value("install", "status"), QStringLiteral("applied"));
        QCOMPARE(st.value("install", "to"), QStringLiteral("2.2.0"));
        QCOMPARE(st.value("install", "sha.tsmedia_win64.dll"), QString::fromLatin1(old64.toHex()));
        QVERIFY(restartPending(box.layout, from));
        QCOMPARE(effectiveInstalled(box.layout, from), to);

        // The new version starts twice without reaching onStarted(): the second start rolls back.
        QCOMPARE(bootGuard(box.layout, to), BootGuard::Continue);
        QCOMPARE(st.value("install", "bootAttempts"), QStringLiteral("1"));
        QCOMPARE(bootGuard(box.layout, to), BootGuard::RolledBack);
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), old64);
        QCOMPARE(sha(box.plugin("tsmedia_win32.dll")), old32);
        QCOMPARE(sha(box.layout.failedDir(to) + QStringLiteral("/tsmedia_win64.dll.bak")), staged.at(0).sha256);
        QCOMPARE(st.value("install", "status"), QStringLiteral("rolledBack"));
        QCOMPARE(bootGuard(box.layout, from), BootGuard::Continue); // the old version runs normally

        // The old version tells the user once.
        StartupNotice notice = onStarted(box.layout, from, 1234);
        QCOMPARE(notice.kind, StartupNotice::RolledBack);
        QCOMPARE(notice.version, to);
        QCOMPARE(notice.restored, from);
        markNoticeShown(box.layout);
        QCOMPARE(onStarted(box.layout, from, 1234).kind, StartupNotice::None);
        QByteArray marker;
        QVERIFY(fs::readAll(box.layout.markerFile(from), &marker, 100));
        QCOMPARE(marker, QByteArray("1234"));
    }

    void successfulStart()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        QVERIFY(applyUpdate(box.layout, staged, from, to).ok());
        QCOMPARE(bootGuard(box.layout, to), BootGuard::Continue);
        StartupNotice notice = onStarted(box.layout, to, 77);
        QCOMPARE(notice.kind, StartupNotice::Updated);
        QCOMPARE(notice.version, to);
        StateFile st(box.layout.stateFile());
        QCOMPARE(st.value("install", "status"), QStringLiteral("done"));
        QCOMPARE(st.value("install", "bootAttempts"), QStringLiteral("0"));
        // Later starts don't count any more.
        QCOMPARE(bootGuard(box.layout, to), BootGuard::Continue);
        QCOMPARE(bootGuard(box.layout, to), BootGuard::Continue);
        QVERIFY(!restartPending(box.layout, to));
        // Not shown yet (no chat open): shown on the next start instead.
        QCOMPARE(onStarted(box.layout, to, 78).kind, StartupNotice::Updated);
        markNoticeShown(box.layout);
        QCOMPARE(onStarted(box.layout, to, 79).kind, StartupNotice::None);
    }

    void failedSwapIsUndone()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        const QByteArray old64 = sha(box.plugin("tsmedia_win64.dll"));
        const QByteArray old32 = sha(box.plugin("tsmedia_win32.dll"));
        // Hold the second staged file open without delete sharing: its move fails after the first
        // file was swapped already.
        HANDLE hold = CreateFileW(fs::native(staged.at(1).path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(hold != INVALID_HANDLE_VALUE);
        const ApplyResult result = applyUpdate(box.layout, staged, from, to);
        CloseHandle(hold);
        QVERIFY(!result.ok());
        QCOMPARE(result.error, ApplyError::MoveFailed);
        QVERIFY(result.unchanged);
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), old64);
        QCOMPARE(sha(box.plugin("tsmedia_win32.dll")), old32);
        QVERIFY(fs::exists(staged.at(0).path)); // back in staging
        StateFile st(box.layout.stateFile());
        QVERIFY(st.value("install", "status") != QStringLiteral("applied"));
        QVERIFY(!restartPending(box.layout, from));
    }

    void stagedFileChanged()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        const QByteArray old64 = sha(box.plugin("tsmedia_win64.dll"));
        fs::removeFile(staged.at(1).path); // "quarantined"
        const ApplyResult result = applyUpdate(box.layout, staged, from, to);
        QCOMPARE(result.error, ApplyError::StagedChanged);
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), old64);
    }

    void loadedDllIsRenamed()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged, false));
        // Map the "running" plugin the way TeamSpeak has it mapped: it can't be overwritten now.
        HMODULE loaded = LoadLibraryExW(fs::native(box.plugin("tsmedia_win64.dll")).c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        QVERIFY(loaded);
        QVERIFY(!DeleteFileW(fs::native(box.plugin("tsmedia_win64.dll")).c_str()));
        const ApplyResult result = applyUpdate(box.layout, staged, from, to);
        QVERIFY(result.ok());
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), staged.at(0).sha256);
        QVERIFY(fs::exists(box.layout.rollbackDir(from) + QStringLiteral("/tsmedia_win64.dll.bak")));
        // The 32-bit DLL wasn't in this update: left alone.
        QVERIFY(fs::exists(box.plugin("tsmedia_win32.dll")));
        FreeLibrary(loaded);
    }

    void updateWhileRestartPending()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        const Version       next = Version::parse(QStringLiteral("2.2.1"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        const QByteArray old64 = sha(box.plugin("tsmedia_win64.dll"));
        QVERIFY(applyUpdate(box.layout, staged, from, to).ok());

        // 2.2.1 arrives before the restart: it replaces the waiting files, the rollback stays 2.1.1.
        QVector<StagedFile> newer;
        const QString       dir = box.layout.stagingDir(next);
        fs::ensureDir(dir);
        StagedFile f;
        f.arch = Arch::Win64;
        f.path = dir + QStringLiteral("/tsmedia_win64.dll.new");
        QVERIFY(writeFakeDll(f.path, false, "newer64"));
        f.sha256 = sha(f.path);
        f.size   = fs::fileSize(f.path);
        newer.append(f);
        QVERIFY(applyUpdate(box.layout, newer, from, next).ok());
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), f.sha256);
        QCOMPARE(sha(box.layout.rollbackDir(from) + QStringLiteral("/tsmedia_win64.dll.bak")), old64);
        QCOMPARE(effectiveInstalled(box.layout, from), next);
        QCOMPARE(StateFile(box.layout.stateFile()).value("install", "from"), QStringLiteral("2.1.1"));
    }

    void restoreChecksHashAndMachine()
    {
        Sandbox             box;
        const Version       from = Version::parse(QStringLiteral("2.1.1"));
        const Version       to   = Version::parse(QStringLiteral("2.2.0"));
        QVector<StagedFile> staged;
        QVERIFY(box.setUp(to, &staged));
        QVERIFY(applyUpdate(box.layout, staged, from, to).ok());
        // Someone swapped the rollback copy: it no longer matches the recorded hash.
        const QString backup = box.layout.rollbackDir(from) + QStringLiteral("/tsmedia_win64.dll.bak");
        QVERIFY(writeFakeDll(backup, false, "planted"));
        QString why;
        QVERIFY(!restorePrevious(box.layout, from, to, &why));
        QVERIFY(!why.isEmpty());
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), staged.at(0).sha256); // nothing moved

        // Matching hash, wrong CPU (an x86 file as the win64 DLL).
        QVERIFY(writeFakeDll(backup, true, "x86"));
        StateFile(box.layout.stateFile()).setValue("install", "sha.tsmedia_win64.dll", QString::fromLatin1(sha(backup).toHex()));
        QVERIFY(!restorePrevious(box.layout, from, to));
        QCOMPARE(sha(box.plugin("tsmedia_win64.dll")), staged.at(0).sha256);
    }

    void cleanupKeepsWhatIsNeeded()
    {
        Sandbox box;
        const Layout& l = box.layout;
        for (const char* v : {"2.0.7", "2.1.0", "2.1.1"}) {
            fs::ensureDir(l.rollbackDir(Version::parse(QString::fromLatin1(v))));
            QVERIFY(fs::writeAll(l.rollbackDir(Version::parse(QString::fromLatin1(v))) + QStringLiteral("/tsmedia_win64.dll.bak"), "x"));
        }
        fs::ensureDir(l.updateDir + QStringLiteral("/rollback/junk"));
        fs::ensureDir(l.stagingDir(Version::parse(QStringLiteral("2.2.0"))));
        fs::ensureDir(l.failedDir(Version::parse(QStringLiteral("2.1.9"))));
        fs::ensureDir(l.downloadDir());
        QVERIFY(fs::writeAll(l.markerFile(Version::parse(QStringLiteral("2.1.0"))), "1"));
        QVERIFY(fs::writeAll(l.markerFile(Version::parse(QStringLiteral("2.2.0"))), "2"));
        StateFile st(l.stateFile());
        st.setValue("install", "status", QStringLiteral("done"));
        st.setValue("install", "from", QStringLiteral("2.1.0"));

        // While an update waits for a restart nothing is touched.
        st.setValue("install", "status", QStringLiteral("applied"));
        cleanup(l, Version::parse(QStringLiteral("2.2.0")), false);
        QVERIFY(fs::isDir(l.stagingDir(Version::parse(QStringLiteral("2.2.0")))));

        st.setValue("install", "status", QStringLiteral("done"));
        cleanup(l, Version::parse(QStringLiteral("2.2.0")), true);
        QVERIFY(!fs::isDir(l.stagingDir(Version::parse(QStringLiteral("2.2.0")))));
        QVERIFY(!fs::isDir(l.updateDir + QStringLiteral("/failed")));
        QVERIFY(fs::isDir(l.downloadDir())); // kept for a manual install
        QVERIFY(!fs::exists(l.markerFile(Version::parse(QStringLiteral("2.1.0")))));
        QVERIFY(fs::exists(l.markerFile(Version::parse(QStringLiteral("2.2.0")))));
        QVERIFY(!fs::isDir(l.rollbackDir(Version::parse(QStringLiteral("2.0.7")))));
        QVERIFY(fs::isDir(l.rollbackDir(Version::parse(QStringLiteral("2.1.0"))))); // referenced
        QVERIFY(fs::isDir(l.rollbackDir(Version::parse(QStringLiteral("2.1.1"))))); // newest
        QVERIFY(!fs::isDir(l.updateDir + QStringLiteral("/rollback/junk")));
        cleanup(l, Version::parse(QStringLiteral("2.2.0")), false);
        QVERIFY(!fs::isDir(l.downloadDir()));
    }

    void noTraceWithoutUpdater()
    {
        // A plugin that never checked for updates (or a self-built one) leaves nothing on disk.
        QTemporaryDir dir;
        Layout        l;
        l.pluginsDir          = dir.path();
        l.updateDir           = dir.path() + QStringLiteral("/tsmedia/update");
        const Version current = Version::parse(QStringLiteral("2.1.0"));
        QCOMPARE(bootGuard(l, current), BootGuard::Continue);
        QCOMPARE(onStarted(l, current, 1).kind, StartupNotice::None);
        cleanup(l, current, false);
        QVERIFY(!fs::exists(l.updateDir));
        QVERIFY(!restartPending(l, current));
        QCOMPARE(effectiveInstalled(l, current), current);
    }

    void updateSettings()
    {
        QTemporaryDir dir;
        const QString file = dir.path() + QStringLiteral("/settings.ini");
        UpdateSettings s   = UpdateSettings::load(file);
        QCOMPARE(int(s.check), int(CheckSetting::NotAsked)); // off until the user agrees
        QCOMPARE(s.timesAsked, 0);
        QVERIFY(!s.skipped.isValid());
        s.check      = CheckSetting::On;
        s.timesAsked = 1;
        s.skipped    = Version::parse(QStringLiteral("2.2.1"));
        s.save(file);
        s = UpdateSettings::load(file);
        QCOMPARE(int(s.check), int(CheckSetting::On));
        QCOMPARE(s.timesAsked, 1);
        QCOMPARE(s.skipped.toString(), QStringLiteral("2.2.1"));
        {
            QSettings raw(file, QSettings::IniFormat);
            raw.setValue(QStringLiteral("updateCheck"), 7);
            raw.setValue(QStringLiteral("updateConsentAsked"), 99);
            raw.setValue(QStringLiteral("updateSkipVersion"), QStringLiteral("2.2.1-evil"));
        }
        s = UpdateSettings::load(file);
        QCOMPARE(int(s.check), int(CheckSetting::NotAsked)); // garbage never means "on"
        QCOMPARE(s.timesAsked, kMaxConsentAsks);
        QVERIFY(!s.skipped.isValid());
    }

    void stateFile()
    {
        QTemporaryDir dir;
        StateFile     st(dir.path() + QStringLiteral("/sub/state.ini"));
        QVERIFY(st.setValue("check", "lastResult", QStringLiteral("upToDate")));
        QCOMPARE(st.value("check", "lastResult"), QStringLiteral("upToDate"));
        const QDateTime t(QDate(2026, 11, 2), QTime(10, 15, 3), Qt::UTC);
        QVERIFY(st.setTime("check", "nextUtc", t));
        QCOMPARE(st.timeValue("check", "nextUtc"), t);
        QVERIFY(st.setValue("check", "failures", QStringLiteral("abc")));
        QCOMPARE(st.intValue("check", "failures", 0, 0, 10), 0);
        QVERIFY(st.setValue("check", "failures", QStringLiteral("9999")));
        QCOMPARE(st.intValue("check", "failures", 0, 0, 10), 10);
        QVERIFY(st.addRevokedKeys({3, 1}));
        QVERIFY(st.addRevokedKeys({1, 7}));
        QCOMPARE(st.revokedKeys(), QSet<int>({1, 3, 7}));
        QCOMPARE(st.value("trust", "revoked"), QStringLiteral("1,3,7"));
        st.setValue("trust", "revoked", QStringLiteral("1,x,300,-2, 5"));
        QCOMPARE(st.revokedKeys(), QSet<int>({1, 5}));
        QVERIFY(st.remove("check", "lastResult"));
        QCOMPARE(st.value("check", "lastResult"), QString());
        QVERIFY(!st.versionValue("install", "to").isValid());
    }
};

TSMEDIA_REGISTER_TEST(TestUpdater)

#include "tst_updater.moc"
