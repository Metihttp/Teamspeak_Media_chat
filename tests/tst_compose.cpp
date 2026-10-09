// Unit tests for the 2.2 send window's rules (composemodel) and hooks (composehooks): what can be sent,
// the texts the window shows, the post order it expects from Core::send, the caption size estimate, the
// pasted-picture file, and the backslash escaping of captions (S0).

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include "albums.h"
#include "composehooks.h"
#include "composemodel.h"
#include "filedrag.h"
#include "spoiler.h"
#include "medialink.h"
#include "testmain.h"

using compose::Item;
using compose::Problem;

namespace {

Item file(const QString& name, qint64 size = 1000, Problem problem = Problem::None)
{
    static int next = 0;
    Item       item;
    item.id       = ++next;
    item.path     = QStringLiteral("C:/files/") + name;
    item.fileName = name;
    item.kind     = kindForFileName(name);
    item.size     = size;
    item.problem  = problem;
    return item;
}

Item pasted(const QSize& size = QSize(1920, 1080))
{
    static int next = 1000;
    Item       item;
    item.id       = ++next;
    item.image    = QImage(size, QImage::Format_RGB32);
    item.fileName = QStringLiteral("Pasted image.png");
    item.kind     = MediaKind::Image;
    item.pixels   = size;
    return item;
}

QVector<Item> images(int count)
{
    QVector<Item> items;
    for (int i = 0; i < count; ++i)
        items << file(QStringLiteral("photo%1.jpg").arg(i));
    return items;
}

// Text as TeamSpeak 3.6.2 shows it (S0): a run of backslashes right before [ or ] is dropped and the
// bracket is shown as text; nothing else changes. Returns the shown text, and whether a tag could open.
QString shownByTeamSpeak(const QString& bbcode, bool* tagOpens)
{
    QString out;
    *tagOpens = false;
    for (int i = 0; i < bbcode.size(); ++i) {
        const QChar ch = bbcode.at(i);
        if (ch == QLatin1Char('\\')) {
            int run = i;
            while (run < bbcode.size() && bbcode.at(run) == QLatin1Char('\\'))
                ++run;
            if (run < bbcode.size() && (bbcode.at(run) == QLatin1Char('[') || bbcode.at(run) == QLatin1Char(']'))) {
                out += bbcode.at(run);
                i = run;
                continue;
            }
            out += bbcode.mid(i, run - i);
            i = run - 1;
            continue;
        }
        if (ch == QLatin1Char('['))
            *tagOpens = true;
        out += ch;
    }
    return out;
}

} // namespace

class TestCompose : public QObject
{
    Q_OBJECT

  private slots:
    void checkFileProblems();
    void problemTexts();
    void spoilerKinds();
    void sendButtonTexts();
    void skippedTexts();
    void metaTexts();
    void albumHints_data();
    void albumHints();
    void postOrderMatchesCore();
    void captionCounter();
    void captionEscapedAsShown_data();
    void captionEscapedAsShown();
    void captionGoesAloneOnlyWhenTooLong();
    void estimatedLinkIsAnUpperBound();
    void spoilerCoverKeepsSize();
    void coverThumbnailFills();
    void pastedImageFiles();
    void hooks();
};

void TestCompose::checkFileProblems()
{
    const qint64 limit = 100LL * 1024 * 1024;
    QCOMPARE(compose::checkFile(true, true, 1, limit), Problem::None);
    QCOMPARE(compose::checkFile(true, true, limit, limit), Problem::None); // at the limit: Core sends it
    QCOMPARE(compose::checkFile(true, true, limit + 1, limit), Problem::TooLarge);
    QCOMPARE(compose::checkFile(true, true, 0, limit), Problem::Empty);
    QCOMPARE(compose::checkFile(false, false, 0, limit), Problem::Missing);
    QCOMPARE(compose::checkFile(true, false, 4096, limit), Problem::Missing); // a folder
}

void TestCompose::problemTexts()
{
    QCOMPARE(compose::problemText(Problem::None, 100), QString());
    QCOMPARE(compose::problemText(Problem::TooLarge, 100), QStringLiteral("Over your 100 MB upload limit"));
    QCOMPARE(compose::problemText(Problem::Empty, 100), QStringLiteral("This file is empty"));
    QVERIFY(compose::problemText(Problem::Unreadable, 100).contains(QStringLiteral("open in another program")));
    QVERIFY(compose::problemText(Problem::Missing, 100).contains(QStringLiteral("moved or deleted")));
}

void TestCompose::spoilerKinds()
{
    QVERIFY(compose::spoilerAllowed(MediaKind::Image));
    QVERIFY(compose::spoilerAllowed(MediaKind::AnimatedImage));
    QVERIFY(compose::spoilerAllowed(MediaKind::Video));
    QVERIFY(!compose::spoilerAllowed(MediaKind::Audio));
    QVERIFY(!compose::spoilerAllowed(MediaKind::Archive));
    QVERIFY(!compose::spoilerAllowed(MediaKind::Document));
    QVERIFY(!compose::spoilerAllowed(MediaKind::Other));
    QVERIFY(compose::isAlbumKind(pasted()));
    QVERIFY(compose::isAlbumKind(file(QStringLiteral("a.gif"))));
    QVERIFY(compose::isAlbumKind(file(QStringLiteral("a.mp4"))));
    QVERIFY(!compose::isAlbumKind(file(QStringLiteral("a.zip"))));
}

void TestCompose::sendButtonTexts()
{
    QCOMPARE(compose::sendButtonText({}), QStringLiteral("Send"));
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.jpg"))}), QStringLiteral("Send"));
    QCOMPARE(compose::sendButtonText(images(3)), QStringLiteral("Send 3 images"));
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.jpg")), file(QStringLiteral("b.gif")), pasted()}), QStringLiteral("Send 3 images"));
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.mp4")), file(QStringLiteral("b.mov"))}), QStringLiteral("Send 2 videos"));
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.mp4")), file(QStringLiteral("b.jpg"))}), QStringLiteral("Send 2 files"));
    // Only what can be sent counts.
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.jpg")), file(QStringLiteral("b.jpg")), file(QStringLiteral("c.zip"), 0, Problem::Empty)}),
             QStringLiteral("Send 2 images"));
    QCOMPARE(compose::sendButtonText({file(QStringLiteral("a.jpg")), file(QStringLiteral("c.zip"), 0, Problem::Empty)}), QStringLiteral("Send"));
    QCOMPARE(compose::sendableCount({file(QStringLiteral("a.jpg")), file(QStringLiteral("c.zip"), 0, Problem::Empty)}), 1);
}

void TestCompose::skippedTexts()
{
    QCOMPARE(compose::skippedText(images(2)), QString());
    // A single item shows its problem in its own place.
    QCOMPARE(compose::skippedText({file(QStringLiteral("a.jpg"), 0, Problem::Empty)}), QString());
    QCOMPARE(compose::skippedText({file(QStringLiteral("a.jpg")), file(QStringLiteral("b.jpg"), 0, Problem::Empty)}),
             QStringLiteral("1 file can't be sent and will be skipped."));
    QCOMPARE(compose::skippedText({file(QStringLiteral("a.jpg")), file(QStringLiteral("b.jpg"), 0, Problem::Empty), file(QStringLiteral("c.jpg"), 0, Problem::Missing)}),
             QStringLiteral("2 files can't be sent and will be skipped."));
    QCOMPARE(compose::skippedText({file(QStringLiteral("a.jpg"), 0, Problem::Empty), file(QStringLiteral("b.jpg"), 9, Problem::TooLarge)}),
             QStringLiteral("None of these files can be sent. You can raise the limit in Settings → Sending."));
}

void TestCompose::metaTexts()
{
    Item photo   = file(QStringLiteral("holiday.jpg"), 2516582);
    photo.pixels = QSize(4032, 3024);
    QCOMPARE(compose::metaText(photo), QStringLiteral("JPG image · 2.4 MB · 4032 × 3024"));
    QCOMPARE(compose::metaText(pasted()), QStringLiteral("1920 × 1080")); // its name says "Pasted image"
    QCOMPARE(compose::accessibleName(pasted()), QStringLiteral("Pasted image, 1920 × 1080"));
    Item clip       = file(QStringLiteral("clip.mp4"), 180LL * 1024 * 1024);
    clip.durationMs = 42000;
    QCOMPARE(compose::metaText(clip), QStringLiteral("MP4 video · 0:42 · 180 MB"));
    QCOMPARE(compose::metaText(file(QStringLiteral("archive.zip"), 12897485)), QStringLiteral("ZIP archive · 12.3 MB"));
    QCOMPARE(compose::metaText(file(QStringLiteral("README"), 10)), QStringLiteral("File · 10 B"));
    // Names from disk are shown without bidi controls (a name can't fake its extension).
    QCOMPARE(compose::displayName(file(QStringLiteral("gpj\u202E.exe"))), QStringLiteral("gpj.exe"));
    QCOMPARE(compose::displayName(pasted()), QStringLiteral("Pasted image"));
    Item marked    = file(QStringLiteral("a.png"), 1024);
    marked.spoiler = true;
    QCOMPARE(compose::accessibleName(marked), QStringLiteral("a.png, PNG image, 1.0 KB, spoiler"));
}

void TestCompose::albumHints_data()
{
    QTest::addColumn<int>("pictures");
    QTest::addColumn<int>("videos");
    QTest::addColumn<QString>("hint");
    QTest::newRow("2") << 2 << 0 << QString();
    QTest::newRow("10") << 10 << 0 << QString();
    QTest::newRow("11") << 11 << 0 << QStringLiteral("11 images will be sent as an album of 10 and one on its own.");
    QTest::newRow("12") << 12 << 0 << QStringLiteral("12 images will be sent as 2 albums (10 + 2).");
    QTest::newRow("21") << 21 << 0 << QStringLiteral("21 images will be sent as 2 albums (10 + 10) and one on its own.");
    QTest::newRow("25") << 25 << 0 << QStringLiteral("25 images will be sent as 3 albums (10 + 10 + 5).");
    QTest::newRow("videos") << 0 << 12 << QStringLiteral("12 videos will be sent as 2 albums (10 + 2).");
    QTest::newRow("mixed") << 6 << 6 << QStringLiteral("12 pictures and videos will be sent as 2 albums (10 + 2).");
}

void TestCompose::albumHints()
{
    QFETCH(int, pictures);
    QFETCH(int, videos);
    QFETCH(QString, hint);
    QVector<Item> items = images(pictures);
    for (int i = 0; i < videos; ++i)
        items << file(QStringLiteral("clip%1.mp4").arg(i));
    items << file(QStringLiteral("notes.pdf")) << file(QStringLiteral("x.jpg"), 0, Problem::Empty); // neither counts
    QCOMPARE(compose::albumHint(items), hint);
    QCOMPARE(compose::albumCandidates(items), pictures + videos);
}

void TestCompose::postOrderMatchesCore()
{
    // [zip, jpg, pdf, mp4, empty jpg, gif]
    const QVector<Item> items = {file(QStringLiteral("a.zip")), file(QStringLiteral("b.jpg")), file(QStringLiteral("c.pdf")),
                                 file(QStringLiteral("d.mp4")), file(QStringLiteral("e.jpg"), 0, Problem::Empty), file(QStringLiteral("f.gif"))};
    const QVector<int> album = compose::postOrder(items, true);
    const QVector<int> plain = compose::postOrder(items, false);
    QCOMPARE(album.size(), 5);
    QCOMPARE(QList<int>(album.begin(), album.end()), (QList<int>{1, 3, 5, 0, 2})); // Core::send: the album first
    QCOMPARE(QList<int>(plain.begin(), plain.end()), (QList<int>{0, 1, 2, 3, 5}));
    // One picture is no album: everything stays where it is.
    const QVector<Item> one    = {file(QStringLiteral("a.zip")), file(QStringLiteral("b.jpg"))};
    const QVector<int>  single = compose::postOrder(one, true);
    QCOMPARE(QList<int>(single.begin(), single.end()), (QList<int>{0, 1}));
}

void TestCompose::captionCounter()
{
    QCOMPARE(compose::captionCounterText(0), QString());
    QCOMPARE(compose::captionCounterText(249), QString());
    QCOMPARE(compose::captionCounterText(250), QStringLiteral("50 left"));
    QCOMPARE(compose::captionCounterText(263), QStringLiteral("37 left"));
    QCOMPARE(compose::captionCounterText(kCaptionMaxChars), QStringLiteral("0 left"));
}

void TestCompose::captionEscapedAsShown_data()
{
    QTest::addColumn<QString>("typed");
    QTest::newRow("plain") << QStringLiteral("Sunset at the lake");
    QTest::newRow("tags") << QStringLiteral("[b]bold?[/b] [i]x[/i] [img]https://tracker.example/p.png[/img]");
    QTest::newRow("fake file") << QStringLiteral("[URL=ts3file://evil?serverUID=a&channel=1&filename=x.png]x[/URL]");
    QTest::newRow("noparse") << QStringLiteral("[noparse][URL=ts3file://x?channel=1&filename=y.png]y[/URL][/noparse]");
    QTest::newRow("brackets only") << QString(kCaptionMaxChars, QLatin1Char('['));
    QTest::newRow("persian") << QStringLiteral("سلام [دنیا] :)");
    QTest::newRow("address") << QStringLiteral("see https://example.com/a?b=[c] and www.example.com");
}

void TestCompose::captionEscapedAsShown()
{
    // Whatever was typed, TeamSpeak shows exactly the sanitized text and opens no tag, and no link is
    // found in it (S0: [noparse] doesn't work in 3.6.2; backslashes do).
    QFETCH(QString, typed);
    const QString clean  = sanitizeCaption(typed);
    const QString bbcode = captionToBBCode(clean);
    bool          opens  = true;
    QCOMPARE(shownByTeamSpeak(bbcode, &opens), clean);
    QVERIFY2(!opens, qPrintable(bbcode));
    QVERIFY(!bbcode.contains(QStringLiteral("[noparse]"), Qt::CaseInsensitive));
    QVERIFY(MediaLink::findInMessage(bbcode).isEmpty());
}

void TestCompose::captionGoesAloneOnlyWhenTooLong()
{
    compose::LinkContext context;
    context.downloadUrl = QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat");
    const QVector<Item> photo = {file(QStringLiteral("holiday.jpg"), 2516582)};
    QVERIFY(!compose::captionGoesAlone(QString(), photo, false, context));
    QVERIFY(!compose::captionGoesAlone(QStringLiteral("   "), photo, false, context));
    QVERIFY(!compose::captionGoesAlone(QStringLiteral("Sunset at the lake"), photo, false, context));

    // Against the composer itself: whenever the estimate says "together", the real message with the same
    // link holds the caption and the link (and the other way round).
    for (int length : {10, 100, 200, 300}) {
        for (const QChar ch : {QChar(QLatin1Char('a')), QChar(0x0633), QChar(QLatin1Char('['))}) {
            const QString caption = QString(length, ch);
            for (bool album : {false, true}) {
                QVector<Item> items = {file(QStringLiteral("خیلی_طولانی_نام_فایل_برای_آزمایش.jpg"), 2516582), file(QStringLiteral("b.mp4"), 99999)};
                const bool    alone = compose::captionGoesAlone(caption, items, album, context);
                const bool    inAlbum = album;
                ComposeOptions options;
                options.caption     = caption;
                options.downloadUrl = context.downloadUrl;
                const QVector<ComposedMessage> messages =
                    composeChatMessagesDetailed({compose::estimatedLink(items.first(), context, inAlbum, 2)}, options);
                QCOMPARE(alone, messages.first().links.isEmpty());
            }
        }
    }
    // Nothing to send: no hint.
    QVERIFY(!compose::captionGoesAlone(QString(300, QChar(0x0633)), {file(QStringLiteral("a.jpg"), 0, Problem::Empty)}, false, context));
}

void TestCompose::estimatedLinkIsAnUpperBound()
{
    compose::LinkContext context;
    Item                 photo = file(QStringLiteral("holiday [1].jpg"), 2516582);
    photo.spoiler              = true;
    const MediaLink link       = compose::estimatedLink(photo, context, true, 4);
    QVERIFY(link.isValid());
    QVERIFY(link.isTsMedia());
    QCOMPARE(link.sha256.size(), 32);
    QCOMPARE(link.previewSha.size(), 16);
    QVERIFY(!link.previewFile.isEmpty());
    QVERIFY(!link.blurHash.isEmpty());
    QVERIFY(link.spoiler);
    QCOMPARE(link.albumCount, 4);
    QCOMPARE(link.fileName, QStringLiteral("holiday__1__3f9a1c2e.jpg")); // Core::makeRemoteName's characters
    // Kept through a round trip: receivers would see the same size.
    QCOMPARE(MediaLink::parse(link.toUrl()).toUrl(), link.toUrl());

    // Files without pictures carry no picture fields, and never a spoiler.
    Item archive          = file(QStringLiteral("a.zip"));
    archive.spoiler       = true;
    const MediaLink plain = compose::estimatedLink(archive, context, true, 4);
    QVERIFY(plain.blurHash.isEmpty() && plain.previewFile.isEmpty() && !plain.spoiler && !plain.hasAlbum());

    // A long name is bounded like Core's (48 characters / 64 bytes before the random part).
    const MediaLink longName = compose::estimatedLink(file(QString(80, QChar(0x0641)) + QStringLiteral(".png")), context, false, 0);
    QVERIFY(longName.fileName.toUtf8().size() <= kRemoteBaseMaxBytes + 13);
    QCOMPARE(compose::estimatedLink(pasted(), context, false, 0).fileName, QStringLiteral("new_photo_3f9a1c2e.png"));
}

void TestCompose::spoilerCoverKeepsSize()
{
    // 2.2 integration: the send window draws the receivers' cover (spoiler::drawCover) on a spoiler's
    // thumbnail, as the chat does: darkened, and fine detail is gone.
    QImage picture(120, 80, QImage::Format_RGB32);
    picture.fill(Qt::white);
    for (int x = 0; x < 120; x += 2)
        picture.setPixelColor(x, 40, Qt::black); // fine detail that must not survive
    bool         blurHash = true;
    const QImage source   = spoiler::coverSource(QString(), QSizeF(picture.size()), picture, false, &blurHash);
    QVERIFY(!blurHash);
    QImage cover(picture.size(), QImage::Format_ARGB32_Premultiplied);
    cover.fill(Qt::transparent);
    QPainter           p(&cover);
    spoiler::CoverLook look;
    look.withPill    = false;
    look.placeholder = Qt::gray;
    spoiler::drawCover(p, QRectF(QPointF(0, 0), QSizeF(picture.size())), source, blurHash, look);
    p.end();
    QCOMPARE(cover.size(), picture.size());
    const QColor middle = cover.pixelColor(60, 20);
    QVERIFY2(middle.red() < 230 && middle.alpha() == 255, qPrintable(QString::number(middle.red())));
    QVERIFY(qAbs(cover.pixelColor(60, 40).red() - cover.pixelColor(61, 40).red()) < 20);
}

void TestCompose::coverThumbnailFills()
{
    QImage wide(400, 100, QImage::Format_RGB32);
    wide.fill(Qt::red);
    const QImage thumb = compose::coverThumbnail(wide, QSize(64, 64));
    QCOMPARE(thumb.size(), QSize(64, 64));
    QCOMPARE(compose::coverThumbnail(wide, QSize(96, 96)).size(), QSize(96, 96));
    QVERIFY(compose::coverThumbnail(QImage(), QSize(64, 64)).isNull());
}

void TestCompose::pastedImageFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = dir.path() + QStringLiteral("/paste");

    QImage small(64, 48, QImage::Format_ARGB32);
    small.fill(QColor(10, 20, 30, 128));
    const QString png = compose::savePastedImage(small, folder, true);
    QVERIFY(!png.isEmpty());
    QVERIFY(QFileInfo(png).fileName().contains(QRegularExpression(QStringLiteral("^new_photo_[0-9a-f]{8}\\.png$"))));
    QImageReader reader(png);
    QCOMPARE(reader.size(), QSize(64, 48));
    QVERIFY(compose::savePastedImage(QImage(), folder, true).isEmpty());

    // A large picture without transparency becomes JPEG (Core::uploadImage's rule), unless switched off.
    if (!QImageWriter::supportedImageFormats().contains("jpg"))
        QSKIP("No JPEG writer in this Qt");
    QImage noisy(1400, 1000, QImage::Format_RGB32);
    quint32 seed = 1;
    for (int y = 0; y < noisy.height(); ++y) {
        auto* line = reinterpret_cast<quint32*>(noisy.scanLine(y));
        for (int x = 0; x < noisy.width(); ++x) {
            seed    = seed * 1664525u + 1013904223u;
            line[x] = 0xff000000u | (seed >> 8);
        }
    }
    const QString jpg = compose::savePastedImage(noisy, folder, true);
    QVERIFY(jpg.endsWith(QStringLiteral(".jpg")));
    QVERIFY(compose::savePastedImage(noisy, folder, false).endsWith(QStringLiteral(".png")));
}

void TestCompose::hooks()
{
    // One own-drag marker for the send window, the chat and drag-out (filedrag.h).
    QMimeData own;
    own.setData(filedrag::mimeFormat(), "key");
    QVERIFY(filedrag::isOwn(&own));
    QCOMPARE(filedrag::mimeFormat(), QStringLiteral("application/x-tsmedia-key"));
    QMimeData other;
    other.setText(QStringLiteral("x"));
    QVERIFY(!filedrag::isOwn(&other));
    QVERIFY(!filedrag::isOwn(nullptr));

    // One album switch: albums::enabled() (on in 2.2).
    QCOMPARE(compose::albumsEnabled(), albums::enabled());
    QVERIFY(compose::albumsEnabled());

    // Without a factory the window shows no presence line.
    QVERIFY(compose::createPresenceLine(nullptr, ChatTarget()) == nullptr);
    int        calls = 0;
    ChatTarget seen;
    compose::setPresenceLineFactory([&calls, &seen](QWidget*, const ChatTarget& target) -> QWidget* {
        ++calls;
        seen = target;
        return nullptr;
    });
    ChatTarget target;
    target.sch      = 7;
    target.mode     = TextMessageTarget_CLIENT;
    target.clientId = 12;
    QVERIFY(compose::createPresenceLine(nullptr, target) == nullptr);
    QCOMPARE(calls, 1);
    QCOMPARE(seen.sch, uint64(7));
    QCOMPARE(seen.clientId, anyID(12));
    compose::setPresenceLineFactory({});
    QVERIFY(compose::createPresenceLine(nullptr, target) == nullptr);
    QCOMPARE(calls, 1);
}

TSMEDIA_REGISTER_TEST(TestCompose)

#include "tst_compose.moc"
