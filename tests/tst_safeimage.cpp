// Unit tests for 2.5: one safe decode path (src/safeimage.*): the format decided by a file's first bytes and forced
// (never SVG or XPM through a file named .jpg / .png), BMP only for peers' and local files, lossless and malformed
// WebP refused (content::webpVerdict). The call-site rule (every decode of a received or website file goes through
// safeimage) belongs to the integration part.

#include <QBuffer>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QImageReader>
#include <QMovie>
#include <QTemporaryDir>
#include <QtTest>

#include "linkfixtures.h"
#include "linkpreview/content.h"
#include "safeimage.h"
#include "testmain.h"

using namespace linkfixtures;
using linkpreview::content::WebpVerdict;

namespace {

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.path() + QLatin1Char('/') + name;
    QFile         file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(bytes);
    return path;
}

QByteArray bmp()
{
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray data;
    QBuffer    buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "BMP");
    return data;
}

} // namespace

class TestSafeImage : public QObject
{
    Q_OBJECT

  private slots:
    void svgAsJpgRefused()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("preview.jpg"), svgAsJpg());
        for (const safeimage::Source source : {safeimage::Source::Website, safeimage::Source::Peer, safeimage::Source::Local}) {
            QVERIFY(safeimage::formatOf(path, source).isEmpty());
            const std::unique_ptr<QImageReader> reader = safeimage::reader(path, source);
            QVERIFY(reader->format().isEmpty());
            QVERIFY(!reader->canRead());
            QVERIFY(reader->read().isNull());
            QVERIFY(!safeimage::movie(path, source, nullptr));
        }
        // What Qt alone would do with the file (content detection): never svg through our reader.
        QImageReader plain(path);
        if (plain.format() == "svg")
            qInfo("Qt's own reader would read this preview.jpg as SVG");
    }

    void xpmAsPngRefused()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("x.png"), xpmImage());
        QVERIFY(safeimage::formatOf(path, safeimage::Source::Peer).isEmpty());
        QVERIFY(!safeimage::reader(path, safeimage::Source::Peer)->canRead());
    }

    void realPicturesRead()
    {
        QTemporaryDir dir;
        const QString pngPath = writeFile(dir, QStringLiteral("a.jpg"), png(5, 4)); // a PNG named .jpg: read as PNG
        QCOMPARE(safeimage::formatOf(pngPath, safeimage::Source::Website), QByteArray("png"));
        const std::unique_ptr<QImageReader> reader = safeimage::reader(pngPath, safeimage::Source::Website);
        QCOMPARE(reader->size(), QSize(5, 4));
        QVERIFY(!reader->read().isNull());
        const QString jpgPath = writeFile(dir, QStringLiteral("b.bin"), jpeg(6, 3));
        QCOMPARE(safeimage::formatOf(jpgPath, safeimage::Source::Website), QByteArray("jpeg"));
        QCOMPARE(safeimage::reader(jpgPath, safeimage::Source::Website)->size(), QSize(6, 3));
        QVERIFY(safeimage::reader(writeFile(dir, QStringLiteral("empty.png"), QByteArray()), safeimage::Source::Website)->format().isEmpty());
        QVERIFY(safeimage::reader(dir.path() + QStringLiteral("/missing.png"), safeimage::Source::Website)->format().isEmpty());
    }

    void bmpOnlyForPeerAndLocal()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("x.png"), bmp());
        QVERIFY(safeimage::formatOf(path, safeimage::Source::Website).isEmpty());
        QCOMPARE(safeimage::formatOf(path, safeimage::Source::Peer), QByteArray("bmp"));
        QCOMPARE(safeimage::formatOf(path, safeimage::Source::Local), QByteArray("bmp"));
        QCOMPARE(safeimage::reader(path, safeimage::Source::Peer)->size(), QSize(3, 2));
    }

    void webpVerdict_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<int>("verdict");
        const int ok = int(WebpVerdict::Ok), lossless = int(WebpVerdict::Lossless), bad = int(WebpVerdict::Malformed);
        QTest::newRow("lossy") << webpLossy() << ok;
        QTest::newRow("vp8l") << webpLossless() << lossless;
        QTest::newRow("alph lossless") << webpAlphLossless() << lossless;
        QTest::newRow("animated lossless frame") << webpAnimatedLosslessFrame() << lossless;
        QByteArray truncated = webpLossy();
        truncated.chop(4);
        QTest::newRow("truncated") << truncated << bad;
        QByteArray chunkTooLong = webpLossless();
        chunkTooLong[16] = static_cast<char>(0xFF); // the VP8L chunk says it is longer than the file
        QTest::newRow("chunk past the end") << chunkTooLong << bad;
        QTest::newRow("not webp") << png(2, 2) << bad;
        QTest::newRow("riff only") << QByteArray("RIFF\x04\x00\x00\x00WEBP", 12) << bad;
        QTest::newRow("empty") << QByteArray() << bad;
        // ALPH with lossy compression (method 0) is fine.
        QByteArray vp8x(10, '\0');
        vp8x[0] = 0x10;
        QByteArray chunks = QByteArray("VP8X\x0a\x00\x00\x00", 8) + vp8x + QByteArray("ALPH\x03\x00\x00\x00\x00\x00\x00\x00", 12)
                            + QByteArray("VP8 \x12\x00\x00\x00", 8) + QByteArray(18, '\x10');
        QByteArray riff = QByteArray("RIFF", 4);
        const quint32 size = static_cast<quint32>(chunks.size() + 4);
        riff.append(static_cast<char>(size & 0xFF)).append(static_cast<char>((size >> 8) & 0xFF)).append(static_cast<char>((size >> 16) & 0xFF)).append(static_cast<char>(size >> 24));
        QTest::newRow("alph lossy") << (riff + "WEBP" + chunks) << ok;
    }
    void webpVerdict()
    {
        QFETCH(QByteArray, file);
        QFETCH(int, verdict);
        QCOMPARE(int(linkpreview::content::webpVerdict(file)), verdict);
    }

    void webpRefusedBySafeimage()
    {
        QTemporaryDir dir;
        for (const QByteArray& bytes : {webpLossless(), webpAlphLossless(), webpAnimatedLosslessFrame()}) {
            const QString path = writeFile(dir, QStringLiteral("x.webp"), bytes);
            QVERIFY(safeimage::formatOf(path, safeimage::Source::Website).isEmpty());
            QVERIFY(safeimage::formatOf(path, safeimage::Source::Peer).isEmpty());
            QVERIFY(!safeimage::movie(path, safeimage::Source::Peer, nullptr));
        }
        const QString lossy = writeFile(dir, QStringLiteral("y.webp"), webpLossy());
        QCOMPARE(safeimage::formatOf(lossy, safeimage::Source::Website), QByteArray("webp"));
    }

    void movieFormatForced()
    {
        QTemporaryDir dir;
        const QString path  = writeFile(dir, QStringLiteral("anim.png"), gif(3)); // a GIF named .png
        QObject       parent;
        QMovie*       movie = safeimage::movie(path, safeimage::Source::Peer, &parent);
        QVERIFY(movie);
        QCOMPARE(movie->format(), QByteArray("gif"));
        QVERIFY(movie->isValid());
        QVERIFY(!safeimage::movie(writeFile(dir, QStringLiteral("still.gif"), png(2, 2)), safeimage::Source::Peer, &parent)); // a PNG: no movie
    }

    // 2.5 (INTEGRATION): every decode of a received or website file goes through safeimage. Content detection only in
    // safeimage itself and for the user's own picked files (the send window, the image editor); QMovie made only here.
    void sourceRuleDecodeSites()
    {
        const QRegularExpression byContent(QStringLiteral("setDecideFormatFromContent\\s*\\(\\s*true\\s*\\)"));
        const QRegularExpression newMovie(QStringLiteral("new\\s+QMovie\\s*\\("));
        const QStringList        contentAllowed{QStringLiteral("safeimage.cpp"), QStringLiteral("composedialog.cpp"), QStringLiteral("imageeditmodel.cpp")};
        QDirIterator             it(QStringLiteral(TSMEDIA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        int                      files = 0;
        while (it.hasNext()) {
            const QString path = it.next();
            const QString name = QFileInfo(path).fileName();
            QFile         f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(f.readAll());
            ++files;
            if (!contentAllowed.contains(name))
                QVERIFY2(!byContent.match(text).hasMatch(), qPrintable(name));
            if (name != QLatin1String("safeimage.cpp"))
                QVERIFY2(!newMovie.match(text).hasMatch(), qPrintable(name));
        }
        QVERIFY(files > 100);
    }
};

TSMEDIA_REGISTER_TEST(TestSafeImage)

#include "tst_safeimage.moc"
