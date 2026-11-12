// 2.5 YouTube player (design2.md 3.2, 4.1 Y-T5, Y-T8): the player helper's delivery. Extraction is content-addressed, a
// tampered file is replaced, nobody can write the file between the check and LoadLibraryExW, stale helpers go, and every
// failing step (no resource, a refused load, a missing export, another API version) makes the player unavailable for the
// session with one log line, Play then opening the browser. Nothing is extracted or loaded before the first Play. The
// WebView2 Runtime check reads the registry only. Stand-in helpers (player/tsmp_stub.cpp) are loaded into this process;
// with the SDK, the real helper too (only tsmp_apiVersion, tsmp_init and tsmp_shutdown: no WebView2 starts).

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include "privacysection.h"
#include "settings.h"
#include "testmain.h"
#include "tsmp.h"
#include "ytplayer/ytbackend.h"
#include "ytplayer/ytplayer.h"

namespace {

QString a(const char* text)
{
    return QString::fromLatin1(text);
}

QByteArray bytesOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int filesIn(const QString& dir)
{
    int          n = 0;
    QDirIterator it(dir, QDir::Files);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

} // namespace

class TestPlayerLoad : public QObject
{
    Q_OBJECT

  private:
    QTemporaryDir tmp;
    QByteArray    stub, stub99;

  private slots:
    void initTestCase()
    {
        QVERIFY(tmp.isValid());
        stub   = bytesOf(QString::fromUtf8(TSMP_STUB_DLL));
        stub99 = bytesOf(QString::fromUtf8(TSMP_STUB_V99_DLL));
        QVERIFY(stub.size() > 1024);
        QVERIFY(stub99.size() > 1024);
        QVERIFY(stub != stub99);
    }
    void cleanupTestCase() { ytbackend::setRegistryReaderForTests(nullptr); }

    // ---- Y-T5 ----
    void contentAddressed()
    {
        const QString name = ytbackend::helperFileName(stub, a("win64"));
        QVERIFY(QRegularExpression(a("^tsmedia_player_win64_[0-9a-f]{16}\\.dll$")).match(name).hasMatch());
        QCOMPARE(ytbackend::helperFileName(stub, a("win64")), name);
        QVERIFY(ytbackend::helperFileName(stub99, a("win64")) != name);
        QVERIFY(ytbackend::helperFileName(stub, a("win32")).startsWith(a("tsmedia_player_win32_")));
    }
    void extractAndLoad()
    {
        const QString dir = tmp.filePath(a("load"));
        bool          writeRefused = false;
        ytbackend::LoadOptions o;
        o.bytes      = stub;
        o.dir        = dir;
        o.arch       = a("win64");
        o.apiVersion = TSMP_API_VERSION;
        // The open-handle rule: while the checked file is held, nobody can write it (until LoadLibraryExW mapped it).
        o.beforeLoad = [&writeRefused](const QString& path) {
            const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
            HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            writeRefused = h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        };
        const ytbackend::LoadResult r = ytbackend::extractAndLoad(o);
        QVERIFY2(r.ok, qPrintable(r.step + QLatin1Char(' ') + QString::number(r.error)));
        QVERIFY(writeRefused);
        QVERIFY(r.module);
        QCOMPARE(QFileInfo(r.path).fileName(), ytbackend::helperFileName(stub, a("win64")));
        QCOMPARE(bytesOf(r.path), stub);
        QVERIFY(!QFile::exists(r.path + a(".part")));
        // Pinned: it stays loaded (no FreeLibrary can unload it).
        HMODULE again = nullptr;
        QVERIFY(GetModuleHandleExW(0, QDir::toNativeSeparators(r.path).toStdWString().c_str(), &again));
        QCOMPARE(static_cast<void*>(again), r.module);
        FreeLibrary(again);
        FreeLibrary(again); // more than it took: a pinned image stays anyway
        QVERIFY(GetModuleHandleW(QDir::toNativeSeparators(r.path).toStdWString().c_str()) != nullptr);
        // A second load (a plugin reload) finds the same file and the same image.
        o.beforeLoad                   = nullptr;
        const ytbackend::LoadResult r2 = ytbackend::extractAndLoad(o);
        QVERIFY(r2.ok);
        QCOMPARE(r2.module, r.module);
    }
    void tamperedReplaced()
    {
        const QString dir  = tmp.filePath(a("tamper"));
        const QString path = QDir(dir).filePath(ytbackend::helperFileName(stub99, a("win64")));
        QVERIFY(QDir().mkpath(dir));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(4096, 'x')); // not ours
        }
        ytbackend::LoadOptions o;
        o.bytes      = stub99;
        o.dir        = dir;
        o.arch       = a("win64");
        o.apiVersion = 99;
        const ytbackend::LoadResult r = ytbackend::extractAndLoad(o);
        QVERIFY2(r.ok, qPrintable(r.step));
        QCOMPARE(bytesOf(path), stub99);
        // The same helper with the plugin's API version: refused (unavailable for the session).
        o.apiVersion                     = TSMP_API_VERSION;
        const ytbackend::LoadResult bad = ytbackend::extractAndLoad(o);
        QVERIFY(!bad.ok);
        QCOMPARE(bad.step, a("version"));
    }
    void failingSteps()
    {
        ytbackend::LoadOptions o;
        o.dir        = tmp.filePath(a("fail"));
        o.arch       = a("win64");
        o.apiVersion = TSMP_API_VERSION;
        // No resource (a build without the SDK).
        QCOMPARE(ytbackend::extractAndLoad(o).step, a("resource"));
        // A file Windows refuses to load (an antivirus or an ASR rule looks the same).
        o.bytes = QByteArray("MZ") + QByteArray(8192, '\x07');
        const ytbackend::LoadResult refused = ytbackend::extractAndLoad(o);
        QVERIFY(!refused.ok);
        QCOMPARE(refused.step, a("load"));
        QVERIFY(refused.error != 0);
        // A DLL without the C API.
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        o.bytes = bytesOf(QString::fromWCharArray(system) + a("\\version.dll"));
        QVERIFY(o.bytes.size() > 1024);
        const ytbackend::LoadResult noApi = ytbackend::extractAndLoad(o);
        QVERIFY(!noApi.ok);
        QCOMPARE(noApi.step, a("export"));
    }
    void staleHelpersGo()
    {
        const QString dir = tmp.filePath(a("stale"));
        QVERIFY(QDir().mkpath(dir));
        const QString keep = ytbackend::helperFileName(stub, a("win64"));
        for (const QString& name : {keep, a("tsmedia_player_win64_0000000000000000.dll"), a("tsmedia_player_win64_1111111111111111.dll.part"),
                                    a("other.dll")}) {
            QFile f(QDir(dir).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        QCOMPARE(ytbackend::deleteStaleHelpers(dir, keep), 2);
        QVERIFY(QFile::exists(QDir(dir).filePath(keep)));
        QVERIFY(QFile::exists(QDir(dir).filePath(a("other.dll"))));
        QCOMPARE(filesIn(dir), 2);
    }
    void backendWithStub()
    {
        HelperBackend backend;
        backend.setSourceForTests(stub, tmp.filePath(a("backend")));
        QString why;
        QVERIFY(backend.load(&why));
        QCOMPARE(backend.runtimeVersion(), a("stub"));
        PlayerOpen o;
        o.page    = a("<!doctype html>");
        o.rect    = QRect(0, 0, 640, 360);
        o.clip    = o.rect;
        const int id = backend.open(o);
        QVERIFY(id > 0);
        const QVector<PlayerEvent> events = backend.poll();
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().player, id);
        QCOMPARE(events.first().kind, int(TSMP_EV_CREATED));
        backend.close(id);
        backend.shutdown();
        QVERIFY(backend.load(&why)); // a plugin reload: tsmp_init again on the same image
        backend.shutdown();
    }
    void refusedLoadIsUnavailable()
    {
        // A refused helper: unavailable for the session, one log line, every Play opens the browser.
        Settings::instance()                    = Settings();
        Settings::instance().youTubeNoticeShown = true;
        ytbackend::setRegistryReaderForTests([](ytbackend::Hive) { return a("157.0.4341.42"); });
        auto* backend = new HelperBackend();
        backend->setSourceForTests(QByteArray("MZ") + QByteArray(4096, '\x01'), tmp.filePath(a("refused")));
        QList<QUrl>         opened;
        YouTubePlayer::Host host;
        host.locate = [](const QString&, YouTubePlayer::Spot* spot) {
            spot->thumb   = QRect(0, 0, 362, 204);
            spot->view    = QRect(0, 0, 800, 600);
            spot->thumbDu = QSizeF(362, 204);
            return true;
        };
        host.openUrl = [&opened](const QUrl& url) { opened << url; };
        YouTubePlayer player(host, std::unique_ptr<PlayerBackend>(backend));
        player.setUserDataFolderForTests(tmp.filePath(a("refused/webview2")));
        QVERIFY(player.playable());
        const QUrl link(a("https://youtu.be/M7lc1UVf-VE"));
        player.play(a("s1"), a("M7lc1UVf-VE"), link);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first(), link);
        QVERIFY(!player.playable());
        QVERIFY(player.diagnosticLines().join(QLatin1Char(' ')).contains(a("load")));
        player.play(a("s1"), a("M7lc1UVf-VE"), link);
        QCOMPARE(opened.size(), 2);
        Settings::instance() = Settings();
    }
    void nothingBeforeFirstPlay()
    {
        // A card drawn with playable true and the Settings dialog opened: the helper file doesn't exist afterwards.
        Settings::instance() = Settings();
        ytbackend::setRegistryReaderForTests([](ytbackend::Hive) { return a("157.0.4341.42"); });
        const QString dir     = tmp.filePath(a("lazy"));
        auto*         backend = new HelperBackend();
        backend->setSourceForTests(stub, dir);
        YouTubePlayer::Host host;
        host.locate = [](const QString&, YouTubePlayer::Spot* spot) {
            spot->thumb   = QRect(0, 0, 362, 204);
            spot->view    = QRect(0, 0, 800, 600);
            spot->thumbDu = QSizeF(362, 204);
            return true;
        };
        host.openUrl = [](const QUrl&) {};
        YouTubePlayer player(host, std::unique_ptr<PlayerBackend>(backend));
        player.setUserDataFolderForTests(dir + a("/webview2"));
        QVERIFY(player.playable());
        QVERIFY(player.cardOf(a("s1"), a("M7lc1UVf-VE")).playable);
        {
            PrivacySection section;
            section.load(Settings::instance());
        }
        QVERIFY(!QDir(dir).exists() || filesIn(dir) == 0);
        QVERIFY(!backend->loadedForTests());
        // The first Play (the notice answered) extracts and loads it.
        player.play(a("s1"), a("M7lc1UVf-VE"), QUrl(a("https://youtu.be/M7lc1UVf-VE")));
        QCOMPARE(player.cardOf(a("s1"), a("M7lc1UVf-VE")).state, YouTubePlayer::State::Consent);
        QVERIFY(!backend->loadedForTests());
        player.consentPlay(a("s1"));
        QVERIFY(backend->loadedForTests());
        QVERIFY(QFile::exists(QDir(dir).filePath(ytbackend::helperFileName(stub, ytbackend::archName()))));
        player.shutdown();
        Settings::instance() = Settings();
    }
    void realHelper()
    {
#ifdef TSMP_REAL_DLL
        // The real helper in this process: only its version, init and shutdown (no WebView2 starts).
        const QByteArray real = bytesOf(QString::fromUtf8(TSMP_REAL_DLL));
        QVERIFY(real.size() > 4096);
        ytbackend::LoadOptions o;
        o.bytes                       = real;
        o.dir                         = tmp.filePath(a("real"));
        o.arch                        = ytbackend::archName();
        o.apiVersion                  = TSMP_API_VERSION;
        const ytbackend::LoadResult r = ytbackend::extractAndLoad(o);
        QVERIFY2(r.ok, qPrintable(r.step));
        HMODULE    m      = static_cast<HMODULE>(r.module);
        const auto init   = reinterpret_cast<tsmp_init_fn>(GetProcAddress(m, "tsmp_init"));
        const auto down   = reinterpret_cast<tsmp_shutdown_fn>(GetProcAddress(m, "tsmp_shutdown"));
        const auto poll   = reinterpret_cast<tsmp_poll_fn>(GetProcAddress(m, "tsmp_poll"));
        QVERIFY(init && down && poll);
        // The offscreen platform never calls OleInitialize: an STA first, or (already MTA) the STA check refuses.
        const HRESULT      co   = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::wstring data = QDir::toNativeSeparators(tmp.filePath(a("real/webview2"))).toStdWString();
        TsmpInit           in{};
        in.userDataFolder = data.c_str();
        in.appOrigin      = L"https://io.github.metihttp.tsmediachat";
        const int rc      = init(&in);
        if (co == RPC_E_CHANGED_MODE)
            QCOMPARE(rc, -2);
        else
            QCOMPARE(rc, 0);
        TsmpEvent events[4];
        QCOMPARE(poll(events, 4), 0);
        // A release with no environment at all (it was never made, or its creation failed): answered at once, so the
        // plugin's poll and a switched-off folder's deletion never wait for the whole session (review fix).
        const auto release = reinterpret_cast<tsmp_releaseEngine_fn>(GetProcAddress(m, "tsmp_releaseEngine"));
        QVERIFY(release);
        if (rc == 0) {
            release();
            QCOMPARE(poll(events, 4), 1);
            QCOMPARE(events[0].player, 0);
            QCOMPARE(events[0].kind, int(TSMP_EV_ENGINEEXITED));
            QCOMPARE(events[0].value, 2);
            QCOMPARE(poll(events, 4), 0);
        }
        down();
        QVERIFY(!QDir(tmp.filePath(a("real/webview2"))).exists()); // init alone starts nothing
        if (SUCCEEDED(co))
            CoUninitialize();
#else
        QSKIP("built without the WebView2 SDK (TSMEDIA_WEBVIEW2_DIR)");
#endif
    }

    // ---- Y-T8 the runtime check ----
    void runtimeCheck()
    {
        using ytbackend::Hive;
        QVERIFY(ytbackend::runtimePresentWith([](Hive h) { return h == Hive::LocalMachine32 ? a("157.0.4341.42") : QString(); }));
        QVERIFY(ytbackend::runtimePresentWith([](Hive h) { return h == Hive::CurrentUser ? a("157.0.4341.42") : QString(); }));
        QVERIFY(!ytbackend::runtimePresentWith([](Hive) { return QString(); }));
        QVERIFY(!ytbackend::runtimePresentWith([](Hive) { return a("0.0.0.0"); }));
        // Cached for the session; read again when the Settings dialog opens.
        int reads = 0;
        ytbackend::setRegistryReaderForTests([&reads](Hive) {
            ++reads;
            return QString();
        });
        QVERIFY(!ytbackend::runtimePresent());
        const int first = reads;
        QCOMPARE(first, 2); // both hives
        QVERIFY(!ytbackend::runtimePresent());
        QCOMPARE(reads, first);
        {
            PrivacySection section; // the Settings dialog opens
            section.load(Settings());
        }
        QCOMPARE(reads, first); // the dialog only marks it for a new read
        QVERIFY(!ytbackend::runtimePresent());
        QCOMPARE(reads, 2 * first);
        QVERIFY(PrivacySection::playYouTubeHint().contains(a("has no YouTube player"))); // this test build has no resource
        ytbackend::setRegistryReaderForTests(nullptr);
        // The real reader touches no file and no DLL: only the registry (this PC has the runtime or not: either is fine).
        const bool real = ytbackend::runtimePresent();
        Q_UNUSED(real);
        QVERIFY(ytbackend::embeddedHelper().isEmpty()); // the test executable carries no helper
        QVERIFY(!ytbackend::helperBuiltIn());
    }
};

TSMEDIA_REGISTER_TEST(TestPlayerLoad)

#include "tst_playerload.moc"
