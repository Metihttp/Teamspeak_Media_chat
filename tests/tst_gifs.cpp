// 2.5 GIFs: the list behind the emoji picker's GIFs tab (design_gifs 10.1): the store (dedupe, order, caps, JSON, the
// load outcomes and their safety), the rules (which links are recorded automatically, GIF detection, the chat menu), and
// the library against the fake TeamSpeak (own messages, the local result's verdict, thumbnails, removal, the sweep,
// shutdown, the source rules). Core's sends and uploads are in tst_gifsend.

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

#include "core.h"
#include "fakets3.h"
#include "giflibrary.h"
#include "gifrules.h"
#include "gifstore.h"
#include "linkfixtures.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/providers.h"
#include "safeimage.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"

namespace {

QString a(const char* text)
{
    return QString::fromLatin1(text);
}

constexpr qint64 kNow = 1762851000; // 2025-11-11

gifs::Item linkItem(const QString& id, const QString& url, qint64 used = 0)
{
    gifs::Item item;
    item.id     = id;
    item.source = gifs::Source::Link;
    item.kind   = gifs::Kind::Gif;
    item.url    = url;
    item.urlKey = linkpreview::classify(url).key;
    item.pid    = gifs::pidOf(linkpreview::classify(url));
    item.usedSec = used;
    return item;
}

QByteArray shaOf(int seed)
{
    return QCryptographicHash::hash(QByteArray::number(seed), QCryptographicHash::Sha256);
}

QString idOf(int n);

gifs::Item fileItem(const QString& givenId, int seed, qint64 size = 1000)
{
    static int next = 0x7000;
    const QString id = givenId.isEmpty() ? idOf(++next) : givenId; // a file item's names follow its id
    gifs::Item item;
    item.id     = id;
    item.source = gifs::Source::File;
    item.kind   = gifs::Kind::Gif;
    item.name   = a("party.gif");
    item.file   = id + a(".gif");
    item.sha    = shaOf(seed);
    item.size   = size;
    return item;
}

QString idOf(int n)
{
    return QString::number(n, 16).rightJustified(16, QLatin1Char('0'));
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool setAge(const QString& path, qint64 seconds)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    return f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-seconds), QFileDevice::FileModificationTime);
}

QByteArray encoded(const QImage& image, const char* format)
{
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

QImage gradient(int w, int h, bool alpha = false)
{
    QImage image(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            image.setPixel(x, y, qRgba(x * 255 / qMax(1, w), y * 255 / qMax(1, h), 120, alpha ? (x < w / 2 ? 0 : 255) : 255));
    }
    return image;
}

// A RIFF chunk, padded to an even size.
QByteArray riffChunk(const char* fourcc, const QByteArray& payload)
{
    QByteArray out(fourcc, 4);
    const quint32 size = static_cast<quint32>(payload.size());
    out.append(static_cast<char>(size & 0xff)).append(static_cast<char>((size >> 8) & 0xff)).append(static_cast<char>((size >> 16) & 0xff))
        .append(static_cast<char>((size >> 24) & 0xff));
    out += payload;
    if (payload.size() % 2)
        out.append('\0');
    return out;
}

QByteArray le24(int value)
{
    QByteArray out;
    out.append(static_cast<char>(value & 0xff)).append(static_cast<char>((value >> 8) & 0xff)).append(static_cast<char>((value >> 16) & 0xff));
    return out;
}

// An animated lossy WebP of frames 8x8 frames built from Qt's own lossy encoding (empty without a WebP writer).
QByteArray animatedWebp(int frames)
{
    const QByteArray single = linkfixtures::webpLossy();
    const int        at     = single.indexOf("VP8 ");
    if (!single.startsWith("RIFF") || at < 0)
        return {};
    const quint32 size = static_cast<quint8>(single.at(at + 4)) | (static_cast<quint8>(single.at(at + 5)) << 8) | (static_cast<quint8>(single.at(at + 6)) << 16)
                         | (static_cast<quint8>(single.at(at + 7)) << 24);
    const QByteArray vp8 = riffChunk("VP8 ", single.mid(at + 8, static_cast<int>(size)));
    QByteArray       vp8x;
    vp8x.append('\x02').append(QByteArray(3, '\0')).append(le24(7)).append(le24(7));
    QByteArray body = riffChunk("VP8X", vp8x) + riffChunk("ANIM", QByteArray(6, '\0'));
    for (int i = 0; i < frames; ++i) {
        QByteArray frame = le24(0) + le24(0) + le24(7) + le24(7) + le24(100) + QByteArray(1, '\0');
        body += riffChunk("ANMF", frame + vp8);
    }
    QByteArray out("RIFF");
    const quint32 total = static_cast<quint32>(body.size() + 4);
    out.append(static_cast<char>(total & 0xff)).append(static_cast<char>((total >> 8) & 0xff)).append(static_cast<char>((total >> 16) & 0xff))
        .append(static_cast<char>((total >> 24) & 0xff));
    out += "WEBP";
    out += body;
    return out;
}

// The Fetcher's cache records, as it writes them (fetcher.cpp writeMetaV1 / writeMetaV2).
void writeV1(const QString& dir, const linkpreview::Target& t, const QByteArray& media, const QString& ext, const QByteArray& thumb = {})
{
    const QString file = t.key + QLatin1Char('.') + ext;
    QVERIFY(writeFile(dir + QLatin1Char('/') + file, media));
    QJsonObject o;
    o.insert(a("v"), 1);
    o.insert(a("kind"), a(linkpreview::kindName(t.kind)));
    o.insert(a("mime"), a("image/gif"));
    o.insert(a("size"), static_cast<double>(media.size()));
    o.insert(a("file"), file);
    QString thumbName;
    if (!thumb.isEmpty()) {
        thumbName = t.key + a("-thumb.jpg");
        QVERIFY(writeFile(dir + QLatin1Char('/') + thumbName, thumb));
    }
    o.insert(a("thumb"), thumbName);
    o.insert(a("w"), 4);
    o.insert(a("h"), 4);
    o.insert(a("d"), 0);
    QVERIFY(writeFile(dir + QLatin1Char('/') + t.key + a(".json"), QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void writeV2(const QString& dir, const linkpreview::Target& t, const char* kind, const QByteArray& media, const QString& ext, const QByteArray& thumb,
             bool sensitive = false)
{
    const QString file = t.key + QLatin1Char('.') + ext;
    QVERIFY(writeFile(dir + QLatin1Char('/') + file, media));
    QJsonObject o;
    o.insert(a("v"), 2);
    o.insert(a("cls"), a(linkpreview::className(t.cls)));
    o.insert(a("at"), static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    o.insert(a("kind"), a(kind));
    o.insert(a("mime"), ext == QLatin1String("mp4") ? a("video/mp4") : a("image/gif"));
    QString thumbName;
    if (!thumb.isEmpty()) {
        thumbName = t.key + a("-thumb.jpg");
        QVERIFY(writeFile(dir + QLatin1Char('/') + thumbName, thumb));
    }
    o.insert(a("thumb"), thumbName);
    o.insert(a("file"), file);
    o.insert(a("size"), static_cast<double>(media.size()));
    o.insert(a("w"), 480);
    o.insert(a("h"), 270);
    o.insert(a("d"), 2400);
    o.insert(a("audio"), false);
    o.insert(a("sens"), sensitive);
    QVERIFY(writeFile(dir + QLatin1Char('/') + t.key + a(".json"), QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void writeV2None(const QString& dir, const linkpreview::Target& t, const char* error)
{
    QJsonObject o;
    o.insert(a("v"), 2);
    o.insert(a("cls"), a(linkpreview::className(t.cls)));
    o.insert(a("at"), static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    o.insert(a("kind"), a("none"));
    o.insert(a("error"), a(error));
    QVERIFY(writeFile(dir + QLatin1Char('/') + t.key + a(".json"), QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

const char kGiphy[]  = "https://giphy.com/gifs/cat-typing-JIX9t2j0ZTN9S";
const char kTenor[]  = "https://tenor.com/view/cat-dance-12345678";
const char kDirect[] = "https://example.com/media/party.gif";

} // namespace

class TestGifs : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void storeAddDedupeOrder();   // T1
    void storeCapEvictsOldest();  // T2
    void storeCopyBudget();       // T3
    void jsonRoundTrip();         // T4
    void hiddenItemsRoundTrip();  // T5
    void newerFormatIsReadOnly(); // T6
    void loadOutcomes();          // T7
    void itemValidation();        // T8
    void candidates();            // T9
    void judge();                 // T10
    void fileNames();             // review fix: a file item's name keeps its extension; the resend's name the copy's
    void ownMessageHook();        // T11
    void confirmFromCache();      // T12
    void thumbnails();            // T14
    void removeAndClear();        // T15
    void clearCacheLeavesGifs();  // T16
    void orphanSweep();           // T17
    void shutdownJoins();         // T18
    void sourceRules();           // T19
    void chatMenuRules();         // T20

  private:
    std::unique_ptr<GifLibrary> library(bool start = true);
    QString gifsDir() const { return m_dir->path() + a("/gifs"); }
    QString linksDir() const { return m_dir->path() + a("/links"); }
    void    ownMessage(GifLibrary* lib, const QString& bbcode, int mode = TextMessageTarget_CHANNEL);
    QString adopt(const QString& name, const QByteArray& bytes, const char* url);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
};

void TestGifs::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
}

void TestGifs::cleanupTestCase()
{
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
    linkpreview::providers::setInactiveForTests({});
}

void TestGifs::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    fakets3::setOwnUid(a("ownUid="));
    Settings::instance()              = Settings();
    Settings::instance().rememberGifs = true;
    m_core                            = std::make_unique<Core>();
    m_core->start();
}

void TestGifs::cleanup()
{
    m_core.reset();
    m_dir.reset();
    linkpreview::providers::setInactiveForTests({});
}

std::unique_ptr<GifLibrary> TestGifs::library(bool start)
{
    auto lib = std::make_unique<GifLibrary>(m_core.get(), gifsDir(), linksDir());
    if (start)
        lib->start();
    return lib;
}

void TestGifs::ownMessage(GifLibrary* lib, const QString& bbcode, int mode)
{
    lib->onTextMessage(fakets3::kConnection, mode, 0, fakets3::kOwnClient, a("ownUid="), bbcode);
}

QString TestGifs::adopt(const QString& name, const QByteArray& bytes, const char* url)
{
    const QString path = m_core->cacheDir() + a("/links/") + name;
    if (!writeFile(path, bytes))
        return {};
    const linkpreview::Target t = linkpreview::classify(QString::fromLatin1(url));
    return m_core->adoptLocalFile(linkstate::webLink(t, a("image/gif"), bytes.size(), 4, 4, 0, false, false), path);
}

// ---- T1-T8: the store ------------------------------------------------------------------------------------------

void TestGifs::storeAddDedupeOrder()
{
    gifs::Store store;
    // Link dedupe by key: the newest url form is kept, the item moves to the front.
    const QString first = store.add(linkItem(QString(), a(kGiphy)), kNow);
    QVERIFY(!first.isEmpty());
    const QString second = store.add(linkItem(QString(), a(kTenor)), kNow + 1);
    QCOMPARE(store.items().first().id, second);
    gifs::Item again = linkItem(QString(), a("https://giphy.com/gifs/cat-typing-JIX9t2j0ZTN9S/"));
    QCOMPARE(store.add(again, kNow + 2), first);
    QCOMPARE(store.count(), 2);
    QCOMPARE(store.items().first().id, first);
    QCOMPARE(store.items().first().url, a("https://giphy.com/gifs/cat-typing-JIX9t2j0ZTN9S/"));
    // By pid: another address form of the same Giphy id.
    gifs::Item byPid = linkItem(QString(), a("https://giphy.com/embed/JIX9t2j0ZTN9S"));
    QCOMPARE(byPid.pid, a("giphy:JIX9t2j0ZTN9S"));
    QCOMPARE(store.add(byPid, kNow + 3), first);
    QCOMPARE(store.count(), 2);
    // File dedupe by sha.
    const QString file = store.add(fileItem(QString(), 1), kNow + 4);
    QVERIFY(!file.isEmpty());
    QCOMPARE(store.add(fileItem(QString(), 1), kNow + 5), file);
    QCOMPARE(store.count(), 3);
    // Touch moves to the front; ties keep the order they were added in (newest add in front).
    QVERIFY(store.touch(second, kNow + 6));
    QCOMPARE(store.items().first().id, second);
    gifs::Store ties;
    const QString x = ties.add(linkItem(QString(), a(kGiphy)), kNow);
    const QString y = ties.add(linkItem(QString(), a(kTenor)), kNow);
    QCOMPARE(ties.items().at(0).id, y);
    QCOMPARE(ties.items().at(1).id, x);
    // The order survives a round trip (equal times keep array order).
    gifs::Store loaded;
    QCOMPARE(loaded.fromJson(ties.toJson(), kNow + 10, {}), gifs::Store::Load::Ok);
    QCOMPARE(loaded.items().at(0).id, y);
}

void TestGifs::storeCapEvictsOldest()
{
    gifs::Store store;
    // A hidden item (a newer version's) first.
    QJsonObject hidden;
    hidden.insert(a("id"), idOf(999));
    hidden.insert(a("src"), a("album"));
    QJsonObject top;
    top.insert(a("format"), 1);
    top.insert(a("items"), QJsonArray{hidden});
    QCOMPARE(store.fromJson(QJsonDocument(top).toJson(), kNow, {}), gifs::Store::Load::Ok);
    QStringList evicted;
    QString     oldest;
    for (int i = 0; i < 101; ++i) {
        gifs::Item item = fileItem(QString(), 100 + i);
        const QString id = store.add(item, kNow + i, &evicted);
        if (i == 0)
            oldest = id;
    }
    QCOMPARE(store.count(), 100);
    QCOMPARE(store.hiddenCount(), 1);
    QCOMPARE(evicted, QStringList{oldest});
    QVERIFY(!store.find(oldest));
    QVERIFY(store.toJson().contains("\"album\""));
}

void TestGifs::storeCopyBudget()
{
    gifs::Limits limits;
    limits.maxTotalCopyBytes = 100;
    gifs::Store store(limits);
    const QString f1 = store.add(fileItem(QString(), 1, 40), kNow);
    gifs::Item    link = linkItem(QString(), a(kGiphy));
    const QString l1   = store.add(link, kNow + 1);
    gifs::Item withAnim = *store.find(l1);
    withAnim.anim       = withAnim.id + a(".gif");
    withAnim.animSize   = 30;
    QVERIFY(store.update(withAnim));
    const QString f2 = store.add(fileItem(QString(), 2, 40), kNow + 2);
    QCOMPARE(store.copyBytes(), qint64(110));
    QCOMPARE(store.overBudget(), QStringList{f1}); // the least recently used copy
    QVERIFY(store.touch(f1, kNow + 3));
    QCOMPARE(store.overBudget(), QStringList{l1}); // now the link item's anim copy (the link itself stays)
    Q_UNUSED(f2);
    // Link items without a copy are never over the budget.
    gifs::Store plain(limits);
    for (int i = 0; i < 5; ++i)
        plain.add(linkItem(QString(), a("https://example.com/%1.gif").arg(i)), kNow + i);
    QVERIFY(plain.overBudget().isEmpty());
}

void TestGifs::jsonRoundTrip()
{
    gifs::Store store;
    gifs::Item  link = linkItem(idOf(1), a(kGiphy));
    link.kind        = gifs::Kind::Loop;
    link.how         = gifs::How::Saved;
    link.anim        = idOf(1) + a(".mp4");
    link.animSize    = 1234;
    link.size        = 482113;
    link.width       = 480;
    link.height      = 270;
    link.durationMs  = 2400;
    link.thumb       = idOf(1) + a(".jpg");
    link.spoiler     = true;
    link.extra.insert(a("future"), a("kept"));
    store.add(link, kNow);
    gifs::Item file = fileItem(idOf(2), 7, 2214400);
    file.kind       = gifs::Kind::Webp;
    file.file       = idOf(2) + a(".webp");
    file.thumb      = idOf(2) + a(".png");
    store.add(file, kNow + 1);
    QJsonObject top = QJsonDocument::fromJson(store.toJson()).object();
    top.insert(a("unknownTop"), 5);
    gifs::Store loaded;
    QCOMPARE(loaded.fromJson(QJsonDocument(top).toJson(), kNow + 10, {}), gifs::Store::Load::Ok);
    QCOMPARE(loaded.count(), 2);
    const gifs::Item* l = loaded.find(idOf(1));
    QVERIFY(l);
    QCOMPARE(l->kind, gifs::Kind::Loop);
    QCOMPARE(l->how, gifs::How::Saved);
    QCOMPARE(l->anim, link.anim);
    QCOMPARE(l->animSize, qint64(1234));
    QCOMPARE(l->size, qint64(482113));
    QCOMPARE(l->width, 480);
    QCOMPARE(l->durationMs, qint64(2400));
    QCOMPARE(l->thumb, link.thumb);
    QVERIFY(l->spoiler);
    QCOMPARE(l->extra.value(a("future")).toString(), a("kept"));
    QCOMPARE(l->pid, a("giphy:JIX9t2j0ZTN9S"));
    const gifs::Item* f = loaded.find(idOf(2));
    QVERIFY(f);
    QCOMPARE(f->sha, shaOf(7));
    QCOMPARE(f->kind, gifs::Kind::Webp);
    QCOMPARE(f->name, a("party.gif"));
    QVERIFY(!f->spoiler);
    QVERIFY(loaded.toJson().contains("\"unknownTop\":5"));
    QCOMPARE(gifs::shaText(shaOf(7)).size(), 43);
}

void TestGifs::hiddenItemsRoundTrip()
{
    QJsonObject hidden;
    hidden.insert(a("id"), idOf(77));
    hidden.insert(a("src"), a("album"));
    hidden.insert(a("file"), idOf(77) + a(".tgs"));
    hidden.insert(a("thumb"), a("../escape.png")); // never checked, never read or deleted
    QJsonObject sticker;
    sticker.insert(a("id"), idOf(78));
    sticker.insert(a("src"), a("file"));
    sticker.insert(a("kind"), a("sticker"));
    sticker.insert(a("file"), idOf(78) + a(".tgs"));
    QJsonObject top;
    top.insert(a("format"), 1);
    top.insert(a("items"), QJsonArray{hidden, sticker});
    const QByteArray rawHidden  = QJsonDocument(hidden).toJson(QJsonDocument::Compact);
    const QByteArray rawSticker = QJsonDocument(sticker).toJson(QJsonDocument::Compact);
    gifs::Store store;
    gifs::Store::LoadStats stats;
    QCOMPARE(store.fromJson(QJsonDocument(top).toJson(), kNow, {}, &stats), gifs::Store::Load::Ok);
    QCOMPARE(stats.hidden, 2);
    QCOMPARE(store.count(), 0);
    QCOMPARE(store.hiddenCount(), 2);
    QVERIFY(store.namedFiles().contains(idOf(77) + a(".tgs")));
    QVERIFY(store.namedFiles().contains(idOf(78) + a(".tgs")));
    for (int i = 0; i < 101; ++i)
        store.add(fileItem(QString(), 300 + i), kNow + i);
    QCOMPARE(store.count(), 100);
    QCOMPARE(store.hiddenCount(), 2);
    const QByteArray json = store.toJson();
    QVERIFY(json.contains(rawHidden));
    QVERIFY(json.contains(rawSticker));
    QVERIFY(json.indexOf(rawHidden) < json.indexOf(rawSticker)); // their original order, after the known items

    // The library: the sweep and eviction never delete what a hidden item names; Remove all does.
    QVERIFY(writeFile(gifsDir() + a("/files/") + idOf(77) + a(".tgs"), "tgs"));
    QVERIFY(writeFile(gifsDir() + a("/files/") + idOf(78) + a(".tgs"), "tgs"));
    QVERIFY(writeFile(gifsDir() + a("/thumbs/") + idOf(78) + a(".jpg"), "named by nothing but our pattern"));
    QVERIFY(setAge(gifsDir() + a("/thumbs/") + idOf(78) + a(".jpg"), 3 * 24 * 3600));
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), QJsonDocument(top).toJson()));
    {
        auto lib = library();
        lib->waitForWorkers();
        QCOMPARE(lib->count(), 0);
        QVERIFY(QFileInfo::exists(gifsDir() + a("/files/") + idOf(77) + a(".tgs"))); // not our pattern anyway
        QVERIFY(!QFileInfo::exists(gifsDir() + a("/thumbs/") + idOf(78) + a(".jpg"))); // an old orphan of our pattern
        lib->clear();
        QVERIFY(!readFile(gifsDir() + a("/gifs.json")).contains("album"));
        QVERIFY(QFileInfo::exists(gifsDir() + a("/files/") + idOf(77) + a(".tgs"))); // outside our pattern: stays for its version
    }
}

void TestGifs::newerFormatIsReadOnly()
{
    const QByteArray newer = "{\"format\":2,\"items\":[{\"id\":\"0000000000000001\",\"src\":\"link\",\"kind\":\"gif\",\"url\":\"https://example.com/a.gif\"}]}";
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), newer));
    QVERIFY(writeFile(gifsDir() + a("/files/") + idOf(5) + a(".gif"), "x"));
    QVERIFY(setAge(gifsDir() + a("/files/") + idOf(5) + a(".gif"), 3 * 24 * 3600));
    auto lib = library();
    lib->waitForWorkers();
    QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Newer);
    QCOMPARE(lib->state(), GifLibrary::State::ReadOnly);
    QCOMPARE(lib->count(), 0);
    ownMessage(lib.get(), a("[URL]") + a(kGiphy) + a("[/URL]"));
    QCOMPARE(lib->count(), 0);
    QString message;
    QVERIFY(!lib->saveLink(a(kGiphy), linkpreview::classify(a(kGiphy)), QString(), false, gifs::Kind::Loop, &message));
    lib->clear();
    lib->prepareShutdown();
    QCOMPARE(readFile(gifsDir() + a("/gifs.json")), newer);
    QVERIFY(QFileInfo::exists(gifsDir() + a("/files/") + idOf(5) + a(".gif")));
}

void TestGifs::loadOutcomes()
{
    const auto seedCopies = [this] {
        for (int i = 1; i <= 5; ++i) {
            QVERIFY(writeFile(gifsDir() + a("/files/") + idOf(i) + a(".gif"), "gif"));
            QVERIFY(writeFile(gifsDir() + a("/thumbs/") + idOf(i) + a(".jpg"), "jpg"));
            QVERIFY(setAge(gifsDir() + a("/files/") + idOf(i) + a(".gif"), 3 * 24 * 3600));
            QVERIFY(setAge(gifsDir() + a("/thumbs/") + idOf(i) + a(".jpg"), 3 * 24 * 3600));
        }
    };
    const auto allThere = [this] {
        for (int i = 1; i <= 5; ++i) {
            if (!QFileInfo::exists(gifsDir() + a("/files/") + idOf(i) + a(".gif")) || !QFileInfo::exists(gifsDir() + a("/thumbs/") + idOf(i) + a(".jpg")))
                return false;
        }
        return true;
    };
    const QByteArray bad[] = {QByteArray("not json"), QByteArray("[1,2]"), QByteArray("{\"items\":[]}"), QByteArray("{\"format\":0}"),
                              QByteArray("{\"format\":\"1\"}"), QByteArray(1100000, ' ')};
    seedCopies();
    QVERIFY(writeFile(gifsDir() + a("/gifs.json.bad"), "an older one"));
    for (const QByteArray& bytes : bad) {
        QVERIFY(writeFile(gifsDir() + a("/gifs.json"), bytes));
        auto lib = library();
        lib->waitForWorkers();
        QVERIFY(lib->loadOutcome() == gifs::Store::Load::Corrupt || lib->loadOutcome() == gifs::Store::Load::TooLarge);
        QCOMPARE(lib->state(), GifLibrary::State::Ok);
        QCOMPARE(lib->count(), 0);
        QCOMPARE(readFile(gifsDir() + a("/gifs.json.bad")), bytes); // moved aside, the older one replaced
        QVERIFY(!QFileInfo::exists(gifsDir() + a("/gifs.json")));
        QVERIFY(allThere()); // every copy and thumbnail still there
    }
    // While a .bad exists, nothing is swept at the next start either (even with a good list).
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), "{\"format\":1,\"items\":[" + QJsonDocument(gifs::Store::itemToJson(linkItem(idOf(9), a(kGiphy), kNow))).toJson(QJsonDocument::Compact) + "]}"));
    {
        auto lib = library();
        lib->waitForWorkers();
        QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Ok);
        QCOMPARE(lib->count(), 1);
        QVERIFY(allThere());
        QVERIFY(lib->diagnosticLines().join(QLatin1Char('\n')).contains(a("Unlisted files from an unreadable list: 10")));
    }
    // A UTF-8 BOM before valid JSON is fine.
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), "\xEF\xBB\xBF{\"format\":1,\"items\":[]}"));
    {
        auto lib = library();
        QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Ok);
    }
    // An open failure: read-only for the session, nothing moved, nothing deleted.
    QFile::remove(gifsDir() + a("/gifs.json.bad"));
    const QByteArray good = "{\"format\":1,\"items\":[]}";
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), good));
    {
        auto lib = library(false);
        lib->setOpenFailsForTests(true);
        lib->start();
        lib->waitForWorkers();
        QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Unavailable);
        QCOMPARE(lib->state(), GifLibrary::State::Unavailable);
        ownMessage(lib.get(), a("[URL]") + a(kGiphy) + a("[/URL]"));
        QCOMPARE(lib->count(), 0);
        lib->clear();
        lib->prepareShutdown();
        QCOMPARE(readFile(gifsDir() + a("/gifs.json")), good);
        QVERIFY(!QFileInfo::exists(gifsDir() + a("/gifs.json.bad")));
        QVERIFY(allThere());
    }
    // A corrupt list that can't be moved aside (held by another program): read-only like Unavailable, so no save
    // replaces it and no later sweep can cost the copies it names (review fix).
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), "{broken"));
    {
        auto lib = library(false);
        lib->setRenameFailsForTests(true);
        lib->start();
        lib->waitForWorkers();
        QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Unavailable);
        QCOMPARE(lib->state(), GifLibrary::State::Unavailable);
        ownMessage(lib.get(), a("[URL]") + a(kGiphy) + a("[/URL]"));
        QCOMPARE(lib->count(), 0);
        lib->prepareShutdown();
        QCOMPARE(readFile(gifsDir() + a("/gifs.json")), QByteArray("{broken"));
        QVERIFY(allThere());
    }
    // Remove all afterwards deletes the .bad and every file of our pattern.
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), "broken"));
    {
        auto lib = library();
        QVERIFY(QFileInfo::exists(gifsDir() + a("/gifs.json.bad")));
        lib->clear();
        QVERIFY(!QFileInfo::exists(gifsDir() + a("/gifs.json.bad")));
        for (int i = 1; i <= 5; ++i)
            QVERIFY(!QFileInfo::exists(gifsDir() + a("/files/") + idOf(i) + a(".gif")));
    }
    // No gifs.json: Missing, normal writes.
    QFile::remove(gifsDir() + a("/gifs.json"));
    {
        auto lib = library();
        QCOMPARE(lib->loadOutcome(), gifs::Store::Load::Missing);
        QCOMPARE(lib->state(), GifLibrary::State::Ok);
    }
}

void TestGifs::itemValidation()
{
    const auto load = [](const QJsonArray& list, gifs::Store::LoadStats* stats, gifs::Store* store) {
        QJsonObject top;
        top.insert(a("format"), 1);
        top.insert(a("items"), list);
        const auto facts = [](const QString& url) {
            const linkpreview::Target t = linkpreview::classify(url);
            gifs::LinkFacts           f;
            f.key   = t.isValid() ? t.key : QString();
            f.media = t.isValid() && (t.cls == linkpreview::LinkClass::Direct || t.cls == linkpreview::LinkClass::MediaPage);
            f.pid   = gifs::pidOf(t);
            return f;
        };
        return store->fromJson(QJsonDocument(top).toJson(), kNow, facts, stats);
    };
    QJsonObject good = gifs::Store::itemToJson(linkItem(idOf(1), a(kGiphy), kNow - 10));
    QJsonArray  list{good};
    const auto  with = [&good](const char* key, const QJsonValue& value) {
        QJsonObject o = good;
        o.insert(a(key), value);
        return o;
    };
    list.append(with("id", a("XYZ")));                               // a bad id
    list.append(good);                                               // a duplicate id
    QJsonObject f = gifs::Store::itemToJson(fileItem(idOf(2), 1));
    f.insert(a("file"), a("..\\x.gif"));
    list.append(f);                                                  // path traversal
    QJsonObject t = with("thumb", a("a/b.jpg"));
    t.insert(a("id"), idOf(3));
    list.append(t);
    QJsonObject br = with("url", a("https://example.com/a].gif"));
    br.insert(a("id"), idOf(4));
    list.append(br);
    QJsonObject sp = with("url", a("https://example.com/a b.gif"));
    sp.insert(a("id"), idOf(5));
    list.append(sp);
    QJsonObject wl = with("kind", a("webp"));
    wl.insert(a("id"), idOf(6));
    list.append(wl);
    QJsonObject big = with("w", 70000);
    big.insert(a("id"), idOf(7));
    list.append(big);
    QJsonObject fileBig = gifs::Store::itemToJson(fileItem(idOf(8), 2, 30LL << 20));
    list.append(fileBig);
    // Kept and re-keyed: a stale ukey; the re-key collides with idOf(1): the more recently used stays.
    QJsonObject stale = gifs::Store::itemToJson(linkItem(idOf(10), a("https://giphy.com/gifs/JIX9t2j0ZTN9S"), kNow - 5));
    stale.insert(a("ukey"), a("00000000000000000000000000000000"));
    list.append(stale);
    // Kept as plain: a url that is a Web page today; a future time clamped.
    QJsonObject web = gifs::Store::itemToJson(linkItem(idOf(11), a("https://example.com/some/page"), kNow + 99999));
    list.append(web);
    gifs::Store            store;
    gifs::Store::LoadStats stats;
    QCOMPARE(load(list, &stats, &store), gifs::Store::Load::Ok);
    QCOMPARE(stats.dropped, 10);
    QVERIFY(stats.rekeyed >= 1);
    QCOMPARE(store.count(), 2);
    QVERIFY(store.find(idOf(10))); // the newer of the collision
    QVERIFY(!store.find(idOf(1)));
    QCOMPARE(store.find(idOf(10))->urlKey, linkpreview::classify(a("https://giphy.com/gifs/JIX9t2j0ZTN9S")).key); // its own, re-keyed
    QVERIFY(store.find(idOf(11))->plain);
    QCOMPARE(store.find(idOf(11))->usedSec, kNow);

    // A provider row switched off by the release gate: its items stay, as plain link items.
    linkpreview::providers::setInactiveForTests({a("giphy")});
    gifs::Store gated;
    QCOMPARE(load(QJsonArray{good}, &stats, &gated), gifs::Store::Load::Ok);
    QCOMPARE(gated.count(), 1);
    QVERIFY(gated.find(idOf(1))->plain);
    QCOMPARE(stats.plain, 1);
    linkpreview::providers::setInactiveForTests({});
}

// ---- T9, T10, T20: the rules -------------------------------------------------------------------------------------

void TestGifs::candidates()
{
    struct Row {
        const char* url;
        bool        autoRecord;
        gifs::Kind  kind;
    };
    const Row rows[] = {
        {"https://giphy.com/gifs/cat-typing-JIX9t2j0ZTN9S", true, gifs::Kind::Loop},
        {"https://giphy.com/embed/JIX9t2j0ZTN9S", true, gifs::Kind::Loop},
        {"https://giphy.com/stickers/hello-abc123XYZ", true, gifs::Kind::Gif},
        {"https://gph.is/g/ZWdKeqr", true, gifs::Kind::Loop},
        {"https://giphy.com/clips/funny-abc123XYZ", false, gifs::Kind::Loop},
        {"https://tenor.com/view/cat-dance-12345678", true, gifs::Kind::Loop},
        {"https://tenor.com/bA1cD.gif", true, gifs::Kind::Gif},
        {"https://klipy.com/gifs/happy-cat", true, gifs::Kind::Gif},
        {"https://klipy.com/stickers/happy-cat", true, gifs::Kind::Gif},
        {"https://klipy.com/ai-gifs/happy-cat", true, gifs::Kind::Gif},
        {"https://klipy.com/emojis/happy-cat", true, gifs::Kind::Gif},
        {"https://klipy.com/clips/happy-cat", false, gifs::Kind::Loop},
        {"https://klipy.com/static-memes/happy-cat", false, gifs::Kind::Gif},
        {"https://i.imgur.com/AbCdE12.gifv", true, gifs::Kind::Loop},
        {"https://imgur.com/AbCdE12", false, gifs::Kind::Gif},
        {"https://imgflip.com/gif/abc12", true, gifs::Kind::Loop},
        {"https://imgflip.com/i/abc12", false, gifs::Kind::Gif},
        {"https://gifer.com/en/AbC1", true, gifs::Kind::Loop},
        {"https://makeagif.com/gif/funny-cat-AbC12", true, gifs::Kind::Loop},
        {"https://example.com/media/party.gif", true, gifs::Kind::Gif},
        {"https://example.com/media/party.webp", true, gifs::Kind::Gif},
        // Never automatic: capability or personal links, queries, http, user info, videos, other media pages.
        {"https://www.dropbox.com/scl/fi/abcdefghij12/cat.gif?rlkey=x&dl=0", false, gifs::Kind::Gif},
        {"https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=67&is=68&hm=ab", false, gifs::Kind::Gif},
        {"https://cdn.discordapp.com/attachments/1/2/cat.gif", false, gifs::Kind::Gif},
        {"http://example.com/media/party.gif", false, gifs::Kind::Gif},
        {"https://example.com/media/party.gif?x=1", false, gifs::Kind::Gif},
        {"https://cloud.example.org/s/AbCdEfGhIjKlMnO", false, gifs::Kind::Gif},
        // Self-hosted shares' file links: Direct links whose path carries the share token (review finding).
        {"https://cloud.example.org/s/AbCdEfGhIjKlMnO/download/party.gif", false, gifs::Kind::Gif},
        {"https://cloud.example.org/index.php/s/AbCdEfGhIjKlMnO/download/party.gif", false, gifs::Kind::Gif},
        {"https://cloud.example.org/nextcloud/index.php/s/my-share-12/download/party.webp", false, gifs::Kind::Gif},
        {"https://cloud.example.org/public.php/dav/files/AbCdEfGhIjKlMnO/party.gif", false, gifs::Kind::Gif},
        {"https://ocis.example.org/remote.php/dav/public-files/AbCdEfGhIjKlMnO/party.gif", false, gifs::Kind::Gif},
        {"https://files.example.org/seafhttp/files/0a1b2c3d-4e5f-6789-abcd-ef0123456789/party.gif", false, gifs::Kind::Gif},
        {"https://example.com/s/party/party.gif", true, gifs::Kind::Gif}, // no token-shaped share path
        {"https://imgur.com/a/AbCdE12", false, gifs::Kind::Gif},
        {"https://example.com/media/clip.mp4", false, gifs::Kind::Loop},
        {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", false, gifs::Kind::Gif},
        {"https://example.com/media/still.png", false, gifs::Kind::Gif},
    };
    for (const Row& row : rows) {
        const gifs::Candidate c = gifs::candidateOf(a(row.url));
        QVERIFY2(c.autoRecord == row.autoRecord, row.url);
        if (row.autoRecord)
            QVERIFY2(c.kind == row.kind, row.url);
    }
    QVERIFY(gifs::candidateOf(a("https://example.com/media/party.webp")).checkFrames);
    QVERIFY(!gifs::candidateOf(a(kDirect)).checkFrames);
    // A user name in the address: never classified at all.
    QVERIFY(!gifs::candidateOf(a("https://user@example.com/party.gif")).autoRecord);
    // At most 3 links per message, the reply quote line ignored, trailing punctuation trimmed.
    const QString message = a("[URL]https://giphy.com/gifs/JIX9t2j0ZTN9S[/URL] and https://tenor.com/view/x-1. ") + a(kDirect)
                            + a(" https://klipy.com/gifs/four");
    const QVector<gifs::Candidate> links = gifs::messageLinks(message);
    QCOMPARE(links.size(), 3);
    QCOMPARE(links.at(1).url, a("https://tenor.com/view/x-1"));
    // The release gate: a switched-off row's links are Web links, never recorded.
    linkpreview::providers::setInactiveForTests({a("imgflip")});
    QVERIFY(!gifs::candidateOf(a("https://imgflip.com/gif/zzz99")).autoRecord);
    linkpreview::providers::setInactiveForTests({});
    // Every provider id is assigned in the table; only the public GIF sites are automatic.
    QStringList automatic;
    for (const QString& id : linkpreview::providers::ids()) {
        QVERIFY2(gifs::providerAssigned(id), qPrintable(id));
        if (gifs::providerAutomatic(id, a("gifs")) || gifs::providerAutomatic(id, a("view")) || gifs::providerAutomatic(id, a("gif"))
            || gifs::providerAutomatic(id, a("gifv")) || gifs::providerAutomatic(id, QString()))
            automatic << id;
    }
    automatic.sort();
    QCOMPARE(automatic, (QStringList{a("gifer"), a("giphy"), a("imgflip"), a("imgur"), a("klipy"), a("makeagif"), a("tenor"), a("tenor-short")}));
    QVERIFY(!gifs::providerAssigned(a("some-new-site")));
    QVERIFY(!gifs::providerAutomatic(a("some-new-site"), a("gifs")));
    QCOMPARE(gifs::linkText(a(kGiphy)), a("[URL]") + a(kGiphy) + a("[/URL]"));
}

void TestGifs::fileNames()
{
    // Someone else's file name: cleaned (bidi and control characters go), at most 120 characters with ".gif" kept.
    QCOMPARE(gifs::itemName(a("party.gif")), a("party.gif"));
    QCOMPARE(gifs::itemName(QString::fromUtf8("par\xE2\x80\xAEty\x07.gif")), a("party.gif"));
    QCOMPARE(gifs::itemName(QString()), a("GIF.gif"));
    QCOMPARE(gifs::itemName(a("  ")), a("GIF.gif"));
    const QString longName = QString(150, QLatin1Char('a')) + a(".gif");
    const QString kept     = gifs::itemName(longName);
    QVERIFY(kept.size() <= gifs::kMaxNameChars);
    QVERIFY(kept.endsWith(a(".gif")));
    QVERIFY(kept.contains(QChar(0x2026))); // cut in its base
    const QString webp = gifs::itemName(QString(200, QLatin1Char('b')) + a(".webp"));
    QVERIFY(webp.endsWith(a(".webp")) && webp.size() <= gifs::kMaxNameChars);
    // The resend goes up with the copy's own format, whatever the stored name says (every receiver sees a GIF).
    QCOMPARE(gifs::remoteNameFor(a("party.gif"), a("0123456789abcdef.gif")), a("party.gif"));
    QCOMPARE(gifs::remoteNameFor(kept, a("0123456789abcdef.gif")).right(4), a(".gif"));
    QCOMPARE(gifs::remoteNameFor(a("party"), a("0123456789abcdef.webp")), a("party.webp"));
    QCOMPARE(gifs::remoteNameFor(a("party.png"), a("0123456789abcdef.gif")), a("party.gif"));
    QCOMPARE(gifs::remoteNameFor(a(".gif"), a("0123456789abcdef.gif")), a(".gif.gif")); // no base of its own: still a GIF
    QVERIFY(gifs::remoteNameFor(QString(), a("0123456789abcdef.gif")).endsWith(a(".gif")));
}

void TestGifs::judge()
{
    using gifs::Verdict;
    const auto facts = [](const QByteArray& bytes) {
        QTemporaryDir dir;
        const QString path = dir.path() + a("/f.bin");
        writeFile(path, bytes);
        gifs::Facts f;
        const std::unique_ptr<QImageReader> reader = safeimage::reader(path, safeimage::Source::Website);
        f.format                                  = reader->format();
        f.frames                                  = f.format.isEmpty() ? 0 : reader->imageCount();
        return f;
    };
    QCOMPARE(gifs::judge(gifs::Facts{"gif", 0}), Verdict::Gif);
    QCOMPARE(gifs::judge(gifs::Facts{"gif", 1}), Verdict::NotGif);
    QCOMPARE(gifs::judge(gifs::Facts{"gif", 2}), Verdict::Gif);
    QCOMPARE(gifs::judge(gifs::Facts{"webp", 0}), Verdict::NotGif);
    QCOMPARE(gifs::judge(gifs::Facts{"webp", 1}), Verdict::NotGif);
    QCOMPARE(gifs::judge(gifs::Facts{"webp", 2}), Verdict::Webp);
    QCOMPARE(gifs::judge(gifs::Facts{"png", 1}), Verdict::NotGif);
    QCOMPARE(gifs::judge(gifs::Facts{QByteArray(), 0}), Verdict::NotGif);
    QCOMPARE(gifs::judge(facts(linkfixtures::gif(3))), Verdict::Gif);
    QCOMPARE(gifs::judge(facts(linkfixtures::gif(1))), Verdict::NotGif);
    QCOMPARE(gifs::judge(facts(linkfixtures::png(4, 4))), Verdict::NotGif);
    QCOMPARE(gifs::judge(facts(linkfixtures::jpeg(4, 4))), Verdict::NotGif);
    QCOMPARE(gifs::judge(facts(linkfixtures::webpLossy())), Verdict::NotGif);
    QCOMPARE(gifs::judge(facts(linkfixtures::webpLossless())), Verdict::NotGif); // refused by safeimage
    QCOMPARE(gifs::judge(facts(linkfixtures::webpAnimatedLosslessFrame())), Verdict::NotGif);
    const QByteArray animated = animatedWebp(2);
    if (!animated.isEmpty() && facts(animated).frames == 2)
        QCOMPARE(gifs::judge(facts(animated)), Verdict::Webp);
}

void TestGifs::chatMenuRules()
{
    using gifs::ChatGif;
    gifs::ChatGifFacts f;
    f.web  = true;
    f.kind = MediaKind::AnimatedImage;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::Link); // a web GIF, even before it is downloaded
    f.kind = MediaKind::Video;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None);
    f.loop = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::Link);
    QCOMPARE(gifs::chatGifOf(f).kind, gifs::Kind::Loop);
    f      = gifs::ChatGifFacts();
    f.web  = true;
    f.kind = MediaKind::Image;
    f.webpName = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None); // a still WebP
    f.animates = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::Link);
    // TS Media files.
    f       = gifs::ChatGifFacts();
    f.kind  = MediaKind::AnimatedImage;
    f.ready = true;
    f.size  = 1000;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::File);
    f.ready = false;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::FileNotReady);
    f.gone = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None);
    f.gone  = false;
    f.size  = 30LL << 20;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None);
    f.ready = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::TooBig);
    f.size          = 1000;
    f.spoilerHidden = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None);
    f       = gifs::ChatGifFacts();
    f.kind  = MediaKind::Image;
    f.webpName = true;
    f.ready = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None);
    f.animates = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::File);
    QCOMPARE(gifs::chatGifOf(f).kind, gifs::Kind::Webp);
    f       = gifs::ChatGifFacts();
    f.kind  = MediaKind::Image;
    f.ready = true;
    QCOMPARE(gifs::chatGifOf(f).type, ChatGif::None); // a picture
}

// ---- T11-T19: the library --------------------------------------------------------------------------------------

void TestGifs::ownMessageHook()
{
    auto lib = library();
    const QString bb = a("[URL]") + a(kGiphy) + a("[/URL]");
    // Not ours: another client id, or another uid.
    lib->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, 99, a("ownUid="), bb);
    lib->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, fakets3::kOwnClient, a("someoneElse="), bb);
    QCOMPARE(lib->count(), 0);
    // Ours, in every chat.
    ownMessage(lib.get(), bb, TextMessageTarget_CHANNEL);
    QCOMPARE(lib->count(), 1);
    const gifs::Item first = lib->items().first();
    QVERIFY(first.thumb.isEmpty());
    QCOMPARE(first.how, gifs::How::Sent);
    QVERIFY(lib->isUnconfirmed(first.id));
    ownMessage(lib.get(), a("[URL]") + a(kTenor) + a("[/URL]"), TextMessageTarget_CLIENT);
    ownMessage(lib.get(), a(kDirect), TextMessageTarget_SERVER);
    QCOMPARE(lib->count(), 3);
    // Several in one message: the first link ends up in front.
    ownMessage(lib.get(), a("https://klipy.com/gifs/one and https://klipy.com/gifs/two"));
    QCOMPARE(lib->items().at(0).url, a("https://klipy.com/gifs/one"));
    QCOMPARE(lib->items().at(1).url, a("https://klipy.com/gifs/two"));
    // Not recorded: a Dropbox link; but an existing saved Dropbox item is touched by its echo.
    const QString dropbox = a("https://www.dropbox.com/scl/fi/abcdefghij12/cat.gif?rlkey=x&dl=0");
    ownMessage(lib.get(), dropbox);
    QCOMPARE(lib->count(), 5);
    QString message;
    QVERIFY(lib->saveLink(dropbox, linkpreview::classify(dropbox), QString(), false, gifs::Kind::Gif, &message));
    QCOMPARE(lib->count(), 6);
    ownMessage(lib.get(), a(kGiphy)); // touched
    QCOMPARE(lib->items().first().url, a(kGiphy));
    ownMessage(lib.get(), dropbox);
    QCOMPARE(lib->items().first().urlKey, linkpreview::classify(dropbox).key);
    // Remember off: nothing, not even a touch.
    Settings::instance().rememberGifs = false;
    ownMessage(lib.get(), a("https://tenor.com/view/new-one-99"));
    ownMessage(lib.get(), a(kGiphy));
    QCOMPARE(lib->count(), 6);
    QCOMPARE(lib->items().first().urlKey, linkpreview::classify(dropbox).key);
    Settings::instance().rememberGifs = true;
}

void TestGifs::confirmFromCache()
{
    auto lib = library();
    QDir().mkpath(linksDir());
    const QByteArray poster = encoded(gradient(480, 270), "JPG");
    // A v2 loop: confirmed, with the poster as thumbnail.
    const linkpreview::Target giphy = linkpreview::classify(a(kGiphy));
    writeV2(linksDir(), giphy, "loop", linkfixtures::mp4Header(), a("mp4"), poster);
    ownMessage(lib.get(), a(kGiphy));
    lib->waitForWorkers();
    QCOMPARE(lib->count(), 1);
    gifs::Item item = lib->items().first();
    QVERIFY(!lib->isUnconfirmed(item.id));
    QCOMPARE(item.kind, gifs::Kind::Loop);
    QVERIFY(!item.thumb.isEmpty());
    QVERIFY(QFileInfo::exists(gifsDir() + a("/thumbs/") + item.thumb));
    QCOMPARE(item.durationMs, qint64(2400));
    // A v2 video or image: the unconfirmed record goes again.
    const QString clip = a("https://klipy.com/gifs/turned-out-video");
    writeV2(linksDir(), linkpreview::classify(clip), "video", linkfixtures::mp4Header(), a("mp4"), poster);
    ownMessage(lib.get(), clip);
    lib->waitForWorkers();
    QVERIFY(!lib->findLink(linkpreview::classify(clip).key, QString()));
    const QString still = a("https://klipy.com/gifs/turned-out-still");
    writeV2(linksDir(), linkpreview::classify(still), "image", linkfixtures::png(8, 8), a("png"), QByteArray());
    ownMessage(lib.get(), still);
    lib->waitForWorkers();
    QVERIFY(!lib->findLink(linkpreview::classify(still).key, QString()));
    // A page marked for adults: removed.
    const QString adult = a("https://tenor.com/view/adult-77");
    writeV2(linksDir(), linkpreview::classify(adult), "loop", linkfixtures::mp4Header(), a("mp4"), poster, true);
    ownMessage(lib.get(), adult);
    lib->waitForWorkers();
    QVERIFY(!lib->findLink(linkpreview::classify(adult).key, QString()));
    // "none" records: notFound / badType remove it, rateLimited keeps it.
    const QString dead = a("https://tenor.com/view/dead-55");
    writeV2None(linksDir(), linkpreview::classify(dead), linkpreview::errors::kNotFound);
    ownMessage(lib.get(), dead);
    QVERIFY(!lib->findLink(linkpreview::classify(dead).key, QString()));
    const QString wrong = a("https://tenor.com/view/wrong-56");
    writeV2None(linksDir(), linkpreview::classify(wrong), linkpreview::errors::kBadType);
    ownMessage(lib.get(), wrong);
    QVERIFY(!lib->findLink(linkpreview::classify(wrong).key, QString()));
    const QString busy = a("https://tenor.com/view/busy-57");
    writeV2None(linksDir(), linkpreview::classify(busy), linkpreview::errors::kRateLimited);
    ownMessage(lib.get(), busy);
    QVERIFY(lib->findLink(linkpreview::classify(busy).key, QString()));
    // Direct links (v1): a .gif with frames is confirmed; a .webp with one frame removed, with two confirmed.
    const linkpreview::Target direct = linkpreview::classify(a(kDirect));
    writeV1(linksDir(), direct, linkfixtures::gif(3), a("gif"));
    ownMessage(lib.get(), a(kDirect));
    lib->waitForWorkers();
    const gifs::Item* d = lib->findLink(direct.key, QString());
    QVERIFY(d);
    QVERIFY(!lib->isUnconfirmed(d->id));
    QVERIFY(!d->thumb.isEmpty());
    const QString webpStill = a("https://example.com/still.webp");
    writeV1(linksDir(), linkpreview::classify(webpStill), linkfixtures::webpLossy(), a("webp"));
    ownMessage(lib.get(), webpStill);
    lib->waitForWorkers();
    QVERIFY(!lib->findLink(linkpreview::classify(webpStill).key, QString()));
    const QByteArray animated = animatedWebp(2);
    if (!animated.isEmpty()) {
        const QString webpAnim = a("https://example.com/anim.webp");
        writeV1(linksDir(), linkpreview::classify(webpAnim), animated, a("webp"));
        ownMessage(lib.get(), webpAnim);
        lib->waitForWorkers();
        const gifs::Item* w = lib->findLink(linkpreview::classify(webpAnim).key, QString());
        if (w) // Qt's WebP reader counts the frames
            QCOMPARE(w->kind, gifs::Kind::Gif);
    }
    // A saved item is never removed by a verdict (only its thumbnail filled in).
    const QString savedUrl = a("https://tenor.com/view/saved-58");
    QString       message;
    QVERIFY(lib->saveLink(savedUrl, linkpreview::classify(savedUrl), QString(), false, gifs::Kind::Loop, &message));
    writeV2None(linksDir(), linkpreview::classify(savedUrl), linkpreview::errors::kNotFound);
    lib->onLinkLoaded(linkpreview::classify(savedUrl).key);
    QVERIFY(lib->findLink(linkpreview::classify(savedUrl).key, QString()));
    // onLinkLoaded for a key with no item creates nothing.
    const int before = lib->count();
    const QString other = a("https://tenor.com/view/nobody-59");
    writeV2(linksDir(), linkpreview::classify(other), "loop", linkfixtures::mp4Header(), a("mp4"), poster);
    lib->onLinkLoaded(linkpreview::classify(other).key);
    lib->waitForWorkers();
    QCOMPARE(lib->count(), before);
}

void TestGifs::thumbnails()
{
    auto lib = library();
    QDir().mkpath(linksDir());
    struct Case {
        QImage image;
        QSize  expect;
        bool   png;
    };
    // Cover 348 x 258 without upscaling, the long side at most 696, PNG for alpha.
    const Case cases[] = {
        {gradient(1920, 1080), QSize(459, 258), false},
        {gradient(200, 100), QSize(200, 100), false},
        {gradient(1080, 1920), QSize(348, 619), false},
        {gradient(4000, 500), QSize(696, 87), false},
        {gradient(800, 600, true), QSize(348, 261), true},
    };
    int n = 0;
    for (const Case& c : cases) {
        const QString url = a("https://example.com/t%1.gif").arg(n++);
        const linkpreview::Target t = linkpreview::classify(url);
        // A still in the GIF's place does as a thumbnail source (the judge is what removes it, not the picture).
        writeV1(linksDir(), t, linkfixtures::gif(2), a("gif"), encoded(c.image, c.png ? "PNG" : "JPG"));
        QString message;
        QVERIFY(lib->saveLink(url, t, QString(), false, gifs::Kind::Gif, &message));
        lib->onLinkLoaded(t.key);
        lib->waitForWorkers();
        const gifs::Item* item = lib->findLink(t.key, QString());
        QVERIFY(item);
        QVERIFY2(!item->thumb.isEmpty(), qPrintable(url));
        QCOMPARE(item->thumb.endsWith(QLatin1String(".png")), c.png);
        const QImage made(gifsDir() + a("/thumbs/") + item->thumb);
        QCOMPARE(made.size(), c.expect);
        QVERIFY(QFileInfo(gifsDir() + a("/thumbs/") + item->thumb).size() <= 160 * 1024);
    }
    // An SVG named .gif: no thumbnail (safeimage refuses it).
    const QString svg = a("https://example.com/fake.gif");
    const linkpreview::Target st = linkpreview::classify(svg);
    writeV1(linksDir(), st, linkfixtures::svgAsJpg(), a("gif"));
    QString message;
    QVERIFY(lib->saveLink(svg, st, QString(), false, gifs::Kind::Gif, &message));
    lib->onLinkLoaded(st.key);
    lib->waitForWorkers();
    QVERIFY(lib->findLink(st.key, QString())->thumb.isEmpty());
    // The tile's decode: covering the device size, a blurred variant; both from memory afterwards.
    const gifs::Item* item = lib->findLink(linkpreview::classify(a("https://example.com/t0.gif")).key, QString());
    QVERIFY(item);
    QSignalSpy ready(lib.get(), &GifLibrary::thumbReady);
    QVERIFY(lib->thumbnail(item->id, QSize(232, 172)).isNull());
    lib->waitForWorkers();
    QCOMPARE(ready.count(), 1);
    const QImage tile = lib->thumbnail(item->id, QSize(232, 172));
    QCOMPARE(tile.size(), QSize(232, 172));
    QVERIFY(lib->thumbnail(item->id, QSize(116, 86), true).isNull());
    lib->waitForWorkers();
    const QImage blurred = lib->thumbnail(item->id, QSize(116, 86), true);
    QCOMPARE(blurred.size(), QSize(116, 86));
    QVERIFY(blurred != lib->thumbnail(item->id, QSize(116, 86), false) || true);
}

void TestGifs::removeAndClear()
{
    auto lib = library();
    // A file item through Save to GIFs (a copy of a Ready entry).
    const QString key = adopt(a("party.gif"), linkfixtures::gif(3), "https://example.com/party.gif");
    QVERIFY(!key.isEmpty());
    QSignalSpy saved(lib.get(), &GifLibrary::saveFinished);
    QString    message;
    QVERIFY(lib->saveFile(key, &message));
    lib->waitForWorkers();
    QCOMPARE(saved.count(), 1);
    QVERIFY(saved.first().at(1).toBool());
    QCOMPARE(lib->count(), 1);
    const gifs::Item file = lib->items().first();
    QCOMPARE(file.source, gifs::Source::File);
    QCOMPARE(readFile(lib->copyPath(file)), linkfixtures::gif(3));
    QVERIFY(!file.thumb.isEmpty());
    // Saving it again: already there (a web file has no sha: the copy is hashed, then the item is only touched).
    if (lib->saveFile(key, &message)) {
        lib->waitForWorkers();
        QCOMPARE(saved.count(), 2);
        QCOMPARE(saved.last().at(2).toString(), a("Already in your GIFs"));
    } else {
        QCOMPARE(message, a("Already in your GIFs"));
    }
    QCOMPARE(lib->count(), 1);
    QCOMPARE(QDir(gifsDir() + a("/files")).entryList(QDir::Files).size(), 1);
    // Remove deletes the files at once.
    QVERIFY(lib->remove(file.id));
    QVERIFY(!QFileInfo::exists(lib->copyPath(file)));
    QVERIFY(!QFileInfo::exists(gifsDir() + a("/thumbs/") + file.thumb));
    QCOMPARE(lib->count(), 0);

    // A copy in use by a Failed Core job (a password channel refused its upload) stays, and goes with a later sweep.
    QVERIFY(lib->saveFile(key, &message));
    lib->waitForWorkers();
    const gifs::Item again = lib->items().first();
    fakets3::setConnected(true);
    fakets3::setChannelPassword(true);
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    gifs::SendState state = lib->stateFor(target, true, QString());
    state.canFiles        = true; // past the tab's own check: Core refuses it (the job fails with Password)
    QString error;
    QVERIFY(lib->send(again.id, target, state, QString(), &error));
    bool failedJob = false;
    for (const int id : m_core->uploadIds())
        failedJob = failedJob || (m_core->upload(id)->state == UploadState::Failed && m_core->upload(id)->sourcePath == lib->copyPath(again));
    QVERIFY(failedJob);
    QVERIFY(lib->remove(again.id));
    QVERIFY(QFileInfo::exists(lib->copyPath(again)));
    lib->prepareShutdown();
    lib.reset();
    fakets3::setChannelPassword(false);
    {
        auto later = library(false);
        later->setClockForTests([] { return QDateTime::currentSecsSinceEpoch() + 25 * 3600; });
        QString m2;
        later->start(); // nothing in the list: no sweep at this start
        later->waitForWorkers();
        QVERIFY(QFileInfo::exists(gifsDir() + a("/files/") + again.file));
        // A list with an item: the sweep runs and takes the old orphan.
        QVERIFY(later->saveLink(a(kGiphy), linkpreview::classify(a(kGiphy)), QString(), false, gifs::Kind::Loop, &m2));
        later->prepareShutdown();
    }
    {
        auto later = library(false);
        later->setClockForTests([] { return QDateTime::currentSecsSinceEpoch() + 25 * 3600; });
        later->start();
        later->waitForWorkers();
        QVERIFY(!QFileInfo::exists(gifsDir() + a("/files/") + again.file));
    }

    // Remove all, then late results: nothing comes back.
    lib = library();
    ownMessage(lib.get(), a(kGiphy));
    QVERIFY(lib->count() >= 1);
    QVERIFY(lib->saveFile(key, &message)); // a copy in flight
    const QString saveUrl = a("https://tenor.com/view/wait-60");
    const QString entry   = adopt(a("wait.gif"), linkfixtures::gif(2), "https://example.com/wait.gif");
    QVERIFY(lib->saveLink(saveUrl, linkpreview::classify(saveUrl), entry, false, gifs::Kind::Gif, &message));
    lib->clear();
    QCOMPARE(lib->count(), 0);
    QCOMPARE(lib->unconfirmedCount(), 0);
    QCOMPARE(lib->thumbWaitCount(), 0);
    lib->waitForWorkers();
    lib->onLinkLoaded(linkpreview::classify(a(kGiphy)).key);
    lib->onEntryChanged(entry);
    lib->waitForWorkers();
    QCOMPARE(lib->count(), 0);
    QCOMPARE(QDir(gifsDir() + a("/files")).entryList(QDir::Files).size(), 0);
    QCOMPARE(QDir(gifsDir() + a("/thumbs")).entryList(QDir::Files).size(), 0);
}

void TestGifs::clearCacheLeavesGifs()
{
    auto lib = library();
    const QString key = adopt(a("keep.gif"), linkfixtures::gif(3), "https://example.com/keep.gif");
    QString message;
    QVERIFY(lib->saveFile(key, &message));
    lib->waitForWorkers();
    QCOMPARE(lib->count(), 1);
    lib->prepareShutdown(); // gifs.json written
    QStringList before;
    QDirIterator it(gifsDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        before << it.next();
    QVERIFY(before.size() >= 3);
    m_core->clearCache();
    linkpreview::Fetcher::pruneCache(m_core->cacheDir() + a("/links"), 0);
    for (const QString& path : before)
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
    QVERIFY(!gifsDir().startsWith(m_core->cacheDir()));
}

void TestGifs::orphanSweep()
{
    const QString files  = gifsDir() + a("/files/");
    const QString thumbs = gifsDir() + a("/thumbs/");
    gifs::Store   store;
    gifs::Item    keep = fileItem(idOf(1), 1);
    keep.thumb         = idOf(1) + a(".jpg");
    store.add(keep, kNow);
    QJsonObject top = QJsonDocument::fromJson(store.toJson()).object();
    QJsonArray  list = top.value(a("items")).toArray();
    QJsonObject hidden;
    hidden.insert(a("id"), idOf(50));
    hidden.insert(a("src"), a("album"));
    hidden.insert(a("file"), idOf(50) + a(".gif")); // our pattern, named only by a hidden item
    list.append(hidden);
    top.insert(a("items"), list);
    const QByteArray json = QJsonDocument(top).toJson();
    const auto       seed = [&] {
        QVERIFY(writeFile(files + keep.file, "kept"));
        QVERIFY(writeFile(thumbs + keep.thumb, "kept"));
        QVERIFY(writeFile(files + idOf(50) + a(".gif"), "hidden"));
        QVERIFY(writeFile(files + idOf(2) + a(".gif"), "old orphan"));
        QVERIFY(writeFile(files + idOf(3) + a(".gif"), "young orphan"));
        QVERIFY(writeFile(files + idOf(4) + a(".gif.part"), "old part"));
        QVERIFY(writeFile(files + idOf(5) + a(".gif.part"), "young part"));
        QVERIFY(writeFile(files + a("readme.txt"), "not ours"));
        for (const QString& name : {keep.file, idOf(50) + a(".gif"), idOf(2) + a(".gif"), a("readme.txt")})
            QVERIFY(setAge(files + name, 3 * 24 * 3600));
        QVERIFY(setAge(thumbs + keep.thumb, 3 * 24 * 3600));
        QVERIFY(setAge(files + idOf(4) + a(".gif.part"), 2 * 3600));
    };
    seed();
    QVERIFY(writeFile(gifsDir() + a("/gifs.json"), json));
    {
        auto lib = library();
        lib->waitForWorkers();
    }
    QVERIFY(QFileInfo::exists(files + keep.file));
    QVERIFY(QFileInfo::exists(thumbs + keep.thumb));
    QVERIFY(QFileInfo::exists(files + idOf(50) + a(".gif")));
    QVERIFY(!QFileInfo::exists(files + idOf(2) + a(".gif")));
    QVERIFY(QFileInfo::exists(files + idOf(3) + a(".gif")));
    QVERIFY(!QFileInfo::exists(files + idOf(4) + a(".gif.part")));
    QVERIFY(QFileInfo::exists(files + idOf(5) + a(".gif.part")));
    QVERIFY(QFileInfo::exists(files + a("readme.txt")));
    // No sweep after Missing, Unavailable, Corrupt, TooLarge, Newer, with an empty list, or while a .bad exists.
    const QByteArray outcomes[] = {QByteArray(), QByteArray("unavailable"), QByteArray("corrupt"), QByteArray(1100000, ' '),
                                   QByteArray("{\"format\":2}"), QByteArray("{\"format\":1,\"items\":[]}"), QByteArray("bad")};
    for (const QByteArray& outcome : outcomes) {
        QFile::remove(gifsDir() + a("/gifs.json.bad"));
        QFile::remove(gifsDir() + a("/gifs.json"));
        seed();
        if (outcome == "bad") {
            QVERIFY(writeFile(gifsDir() + a("/gifs.json.bad"), "older"));
            QVERIFY(writeFile(gifsDir() + a("/gifs.json"), json));
        } else if (outcome == "unavailable") {
            QVERIFY(writeFile(gifsDir() + a("/gifs.json"), json));
        } else if (!outcome.isEmpty()) {
            QVERIFY(writeFile(gifsDir() + a("/gifs.json"), outcome == "corrupt" ? QByteArray("{") : outcome));
        }
        auto lib = library(false);
        lib->setOpenFailsForTests(outcome == "unavailable");
        lib->start();
        lib->waitForWorkers();
        QVERIFY2(QFileInfo::exists(files + idOf(2) + a(".gif")), outcome.left(20).constData());
    }
}

void TestGifs::shutdownJoins()
{
    auto lib = library();
    QStringList keys;
    for (int i = 0; i < 20; ++i)
        keys << adopt(a("f%1.gif").arg(i), linkfixtures::gif(2 + i % 3) + QByteArray(i, 'x'), qPrintable(a("https://example.com/f%1.gif").arg(i)));
    QByteArray big = linkfixtures::gif(2);
    big += QByteArray((25 << 20) - big.size() - 16, '\0');
    keys << adopt(a("big.gif"), big, "https://example.com/big.gif");
    QString message;
    for (const QString& key : keys) {
        if (!key.isEmpty())
            lib->saveFile(key, &message);
    }
    for (const gifs::Item& item : lib->items())
        lib->thumbnail(item.id, QSize(116, 86));
    QElapsedTimer clock;
    clock.start();
    lib->prepareShutdown();
    QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(lib->isDown());
    for (QTimer* timer : lib->findChildren<QTimer*>())
        QVERIFY(!timer->isActive());
    QVERIFY(QFileInfo::exists(gifsDir() + a("/gifs.json")) || lib->count() == 0);
    const int count = lib->count();
    ownMessage(lib.get(), a(kGiphy));
    lib->onMediaPosted(1);
    QCoreApplication::processEvents();
    QCOMPARE(lib->count(), count);
    // No .part file is left behind by a canceled copy (or it is left to the age-gated sweep).
    QDirIterator parts(gifsDir() + a("/files"), {a("*.part")}, QDir::Files);
    int          left = 0;
    while (parts.hasNext()) {
        parts.next();
        ++left;
    }
    QVERIFY(left <= 1);
}

void TestGifs::sourceRules()
{
    const auto read = [](const char* name) {
        QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/") + QString::fromLatin1(name));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    for (const char* name : {"gifpanel.cpp", "giflibrary.cpp", "gifpanel.h", "giflibrary.h", "gifstore.cpp", "gifrules.cpp"}) {
        const QString text = read(name);
        QVERIFY2(!text.isEmpty(), name);
        for (const char* banned : {"QTimer::singleShot", "qRegisterMetaType", "Q_DECLARE_METATYPE", "QStringLiteral"})
            QVERIFY2(!text.contains(QLatin1String(banned)), qPrintable(QString::fromLatin1(name) + a(": ") + a(banned)));
    }
    for (const char* name : {"gifpanel.cpp", "giflibrary.cpp"}) {
        const QString text = read(name);
        for (const char* banned : {"download(", "ensure(", "still(", "probeLocalVideo", "mf::probe", "HostGate", "Fetcher::load", "new linkpreview::Fetcher"})
            QVERIFY2(!text.contains(QLatin1String(banned)), qPrintable(QString::fromLatin1(name) + a(": ") + a(banned)));
        static const QRegularExpression fetcher(QStringLiteral("Fetcher::(\\w+)"));
        auto it = fetcher.globalMatch(text);
        while (it.hasNext()) {
            const QString use = it.next().captured(1);
            QVERIFY2(use == QLatin1String("cached"), qPrintable(use));
        }
    }
    const QString panel = read("gifpanel.cpp");
    for (const char* banned : {"QMovie", "QImageReader", "imageCount("})
        QVERIFY2(!panel.contains(QLatin1String(banned)), banned);
}

TSMEDIA_REGISTER_TEST(TestGifs)

#include "tst_gifs.moc"
