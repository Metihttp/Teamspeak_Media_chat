// Unit tests for the 2.2 picture editor's model (imageeditmodel): rotation and crop math, undo/redo,
// export sizes and exactness at full resolution, pixelate and black box, the crop helpers, the export
// format rules (PNG/JPEG, no EXIF) and opening pictures (EXIF orientation, animated, too large).
// Text shapes need fonts (testmain's QGuiApplication): their size is checked here, their look in the
// render harness.

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

#include "imageeditmodel.h"
#include "testmain.h"

using imageedit::Doc;
using imageedit::Handle;
using imageedit::ImageEditModel;
using imageedit::Shape;
using imageedit::ShapeType;

namespace {

// Every pixel different: (x, y) -> r = x, g = y, b = x ^ y.
QImage pattern(int w, int h, bool alpha = false)
{
    QImage image(w, h, alpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            image.setPixel(x, y, qRgba(x & 0xff, y & 0xff, (x ^ y) & 0xff, 255));
    }
    return image;
}

ImageEditModel modelOf(const QImage& image)
{
    ImageEditModel model;
    model.setBase(image);
    return model;
}

Shape rectShape(ShapeType type, const QRectF& rect, const QColor& color = Qt::red, qreal width = 4)
{
    Shape s;
    s.type  = type;
    s.rect  = rect;
    s.color = color;
    s.width = width;
    return s;
}

Shape penShape(const QVector<QPointF>& points, qreal width = 4)
{
    Shape s;
    s.type   = ShapeType::Pen;
    s.points = points;
    s.color  = Qt::blue;
    s.width  = width;
    return s;
}

quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xffffffffU;
    for (char ch : data) {
        crc ^= static_cast<quint8>(ch);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

QByteArray be32(quint32 v)
{
    QByteArray out(4, '\0');
    out[0] = char((v >> 24) & 0xff);
    out[1] = char((v >> 16) & 0xff);
    out[2] = char((v >> 8) & 0xff);
    out[3] = char(v & 0xff);
    return out;
}

// A PNG whose header claims width x height (its pixel data is a stub).
QByteArray pngHeaderOnly(quint32 width, quint32 height)
{
    QByteArray ihdr = QByteArray("IHDR") + be32(width) + be32(height) + QByteArray("\x08\x02\x00\x00\x00", 5);
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    png += be32(13) + ihdr + be32(crc32(ihdr));
    QByteArray idat = QByteArray("IDAT") + QByteArray("\x78\x9c\x03\x00\x00\x00\x00\x01", 8); // never decoded
    png += be32(8) + idat + be32(crc32(idat));
    QByteArray iend("IEND");
    png += be32(0) + iend + be32(crc32(iend));
    return png;
}

// A 1 x 1 GIF with two frames.
QByteArray twoFrameGif()
{
    QByteArray gif("GIF89a", 6);
    gif += QByteArray("\x01\x00\x01\x00\x80\x00\x00", 7);       // 1 x 1, a 2-colour table
    gif += QByteArray("\x00\x00\x00\xff\xff\xff", 6);           // black, white
    const QByteArray frame = QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8) // 0.1 s
                             + QByteArray("\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00", 10)
                             + QByteArray("\x02\x02\x44\x01\x00", 5);
    gif += frame + frame;
    gif += QByteArray("\x3b", 1);
    return gif;
}

// A JPEG with an EXIF block saying "rotate 90° clockwise to show" (orientation 6).
QByteArray jpegWithOrientation6(const QImage& stored)
{
    QByteArray jpeg;
    QBuffer    buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    stored.save(&buffer, "JPG", 95);
    buffer.close();
    QByteArray tiff("II\x2a\x00\x08\x00\x00\x00", 8);              // little endian, IFD at 8
    tiff += QByteArray("\x01\x00", 2);                              // one entry
    tiff += QByteArray("\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00", 12); // Orientation = 6
    tiff += QByteArray("\x00\x00\x00\x00", 4);                      // no next IFD
    const QByteArray payload = QByteArray("Exif\x00\x00", 6) + tiff;
    QByteArray       app1("\xff\xe1", 2);
    const int        length = payload.size() + 2;
    app1 += char((length >> 8) & 0xff);
    app1 += char(length & 0xff);
    app1 += payload;
    return jpeg.left(2) + app1 + jpeg.mid(2); // right after SOI
}

bool isRed(QRgb c)
{
    return qRed(c) > 180 && qGreen(c) < 80 && qBlue(c) < 80;
}

bool isBlue(QRgb c)
{
    return qBlue(c) > 180 && qRed(c) < 80 && qGreen(c) < 80;
}

} // namespace

class TestImageEdit : public QObject
{
    Q_OBJECT

  private slots:
    void rotationMapsCorners();
    void rectsRoundTrip();
    void cropAndRotateSizes();
    void exportCropIsExact();
    void exportRotationIsExact();
    void exportNeverScales();
    void exportKeepsAlpha();
    void setCropClamps();
    void undoRedo();
    void undoRedoLimitAndNoops();
    void clearIsUndoable();
    void setBaseKeepsOrResets();
    void viewDoesNotChangeExport();
    void hiDpiScreenMatchesExport();
    void strokeWidthFromScreen();
    void textSizeIsNotRounded();
    void shapesOrderPixelateCoversEarlier();
    void pixelBlockRule();
    void pixelateIsBlockyQuantizedAndFixed();
    void pixelateSameAtAnyOffset();
    void blackBoxIsOpaque();
    void fitAspectCases();
    void dragCropFree();
    void dragCropAspect();
    void snapAndSquare();
    void transparency();
    void encodeFormats();
    void exportNames();
    void writeEditedFailsCleanly();
    void exifOrientationAndStripping();
    void openProblems();
    void palette();
};

void TestImageEdit::rotationMapsCorners()
{
    const QSize size(40, 30);
    // 90: (x, y) -> (h - y, x); 180: (w - x, h - y); 270: (y, w - x).
    QCOMPARE(imageedit::rotationTransform(90, size).map(QPointF(0, 0)), QPointF(30, 0));
    QCOMPARE(imageedit::rotationTransform(90, size).map(QPointF(40, 30)), QPointF(0, 40));
    QCOMPARE(imageedit::rotationTransform(180, size).map(QPointF(0, 0)), QPointF(40, 30));
    QCOMPARE(imageedit::rotationTransform(270, size).map(QPointF(0, 0)), QPointF(0, 40));
    QCOMPARE(imageedit::rotationTransform(270, size).map(QPointF(40, 30)), QPointF(30, 0));
    QCOMPARE(imageedit::rotationTransform(0, size).map(QPointF(7, 9)), QPointF(7, 9));
    QCOMPARE(imageedit::rotationTransform(360 + 90, size).map(QPointF(0, 0)), QPointF(30, 0));
}

void TestImageEdit::rectsRoundTrip()
{
    const QSize size(4032, 3024);
    const QRect rects[] = {QRect(0, 0, 4032, 3024), QRect(10, 20, 300, 200), QRect(4031, 3023, 1, 1), QRect(1000, 0, 16, 3024)};
    for (int rotation : {0, 90, 180, 270}) {
        for (const QRect& r : rects) {
            const QRect rotated = imageedit::baseToRotated(r, rotation, size);
            QCOMPARE(rotated.size(), rotation % 180 ? r.size().transposed() : r.size());
            QCOMPARE(imageedit::rotatedToBase(rotated, rotation, size), r);
        }
    }
    // 90°: the base's top left pixel is the rotated picture's top right one.
    QCOMPARE(imageedit::baseToRotated(QRect(0, 0, 1, 1), 90, size), QRect(3023, 0, 1, 1));
}

void TestImageEdit::cropAndRotateSizes()
{
    ImageEditModel model = modelOf(pattern(400, 300));
    QCOMPARE(model.outputSize(), QSize(400, 300));
    QVERIFY(!model.isCropped());
    model.setCrop(QRect(10, 20, 200, 100));
    QVERIFY(model.isCropped());
    QCOMPARE(model.outputSize(), QSize(200, 100));
    QCOMPARE(model.exportImage().size(), QSize(200, 100));
    model.rotateClockwise();
    QCOMPARE(model.rotation(), 90);
    QCOMPARE(model.rotatedSize(), QSize(300, 400));
    QCOMPARE(model.outputSize(), QSize(100, 200));
    QCOMPARE(model.exportImage().size(), QSize(100, 200));
    // The crop is kept in base pixels: in the rotated picture it moved with it.
    QCOMPARE(model.crop(), QRect(10, 20, 200, 100));
    QCOMPARE(model.cropInRotated(), QRect(300 - 120, 10, 100, 200));
    model.rotateClockwise();
    model.rotateClockwise();
    model.rotateClockwise();
    QCOMPARE(model.rotation(), 0);
    QCOMPARE(model.outputSize(), QSize(200, 100));
}

void TestImageEdit::exportCropIsExact()
{
    const QImage   base  = pattern(300, 200);
    ImageEditModel model = modelOf(base);
    const QRect    crop(17, 33, 120, 90);
    model.setCrop(crop);
    const QImage out = model.exportImage();
    QCOMPARE(out.size(), crop.size());
    QCOMPARE(out.convertToFormat(QImage::Format_RGB32), base.copy(crop).convertToFormat(QImage::Format_RGB32));
}

void TestImageEdit::exportRotationIsExact()
{
    const QImage base = pattern(64, 40);
    for (int turns = 1; turns <= 3; ++turns) {
        ImageEditModel model = modelOf(base);
        for (int i = 0; i < turns; ++i)
            model.rotateClockwise();
        const QImage out = model.exportImage();
        const int    w   = base.width();
        const int    h   = base.height();
        for (int y = 0; y < h; y += 7) {
            for (int x = 0; x < w; x += 5) {
                QPoint at;
                if (turns == 1)
                    at = QPoint(h - 1 - y, x);
                else if (turns == 2)
                    at = QPoint(w - 1 - x, h - 1 - y);
                else
                    at = QPoint(y, w - 1 - x);
                QCOMPARE(out.pixel(at), base.pixel(x, y));
            }
        }
        // The same through the model's own transform (pixel centres).
        const QPointF centre = model.baseToOutput().map(QPointF(3.5, 2.5));
        QCOMPARE(out.pixel(int(centre.x()), int(centre.y())), base.pixel(3, 2));
    }
}

void TestImageEdit::exportNeverScales()
{
    // A large picture keeps its pixels: the export is the crop at 1:1, whatever was shown.
    ImageEditModel model = modelOf(pattern(2000, 1500));
    model.setCrop(QRect(100, 100, 1600, 900));
    const QImage out = model.exportImage();
    QCOMPARE(out.size(), QSize(1600, 900));
    QCOMPARE(out.pixel(0, 0), pattern(2000, 1500).pixel(100, 100));
}

void TestImageEdit::exportKeepsAlpha()
{
    QImage base(50, 40, QImage::Format_ARGB32);
    base.fill(Qt::transparent);
    base.setPixel(10, 10, qRgba(255, 0, 0, 255));
    ImageEditModel model = modelOf(base);
    QVERIFY(model.base().hasAlphaChannel());
    model.addShape(rectShape(ShapeType::Rect, QRectF(20, 20, 10, 10)));
    const QImage out = model.exportImage();
    QVERIFY(out.hasAlphaChannel());
    QCOMPARE(qAlpha(out.pixel(0, 0)), 0);
    QCOMPARE(out.pixel(10, 10), qRgba(255, 0, 0, 255));
    // An opaque picture stays opaque.
    QVERIFY(!modelOf(pattern(10, 10)).exportImage().hasAlphaChannel());
}

void TestImageEdit::setCropClamps()
{
    ImageEditModel model = modelOf(pattern(400, 300));
    model.setCrop(QRect(-50, -50, 200, 200)); // partly outside
    QCOMPARE(model.crop(), QRect(0, 0, 150, 150));
    model.setCrop(QRect(390, 290, 3, 3)); // too small: grown to the minimum, inside the picture
    QCOMPARE(model.crop().size(), QSize(imageedit::kMinCropSide, imageedit::kMinCropSide));
    QVERIFY(QRect(0, 0, 400, 300).contains(model.crop()));
    model.setCrop(QRect(0, 0, 400, 300)); // the whole picture: not cropped
    QVERIFY(!model.isCropped());
    QVERIFY(model.doc().crop.isEmpty());
    const int steps = model.undoCount();
    model.setCrop(QRect(500, 500, 10, 10)); // nowhere on the picture: ignored
    QCOMPARE(model.undoCount(), steps);
    // A tiny picture: the minimum is its own side.
    ImageEditModel tiny = modelOf(pattern(10, 8));
    tiny.setCrop(QRect(2, 2, 3, 3));
    QCOMPARE(tiny.crop(), QRect(0, 0, 10, 8));
}

void TestImageEdit::undoRedo()
{
    ImageEditModel model = modelOf(pattern(200, 100));
    QVERIFY(!model.isModified());
    QVERIFY(!model.canUndo());
    QVector<Doc> states{model.doc()};
    model.addShape(penShape({QPointF(1, 1), QPointF(50, 50), QPointF(90, 20)}));
    states << model.doc();
    model.setCrop(QRect(10, 10, 100, 60));
    states << model.doc();
    model.rotateClockwise();
    states << model.doc();
    model.addShape(rectShape(ShapeType::BlackBox, QRectF(20, 20, 30, 10)));
    states << model.doc();
    QVERIFY(model.isModified());
    QCOMPARE(model.undoCount(), 4);

    for (int i = states.size() - 2; i >= 0; --i) {
        QVERIFY(model.undo());
        QVERIFY(model.doc() == states.at(i));
    }
    QVERIFY(!model.undo());
    QVERIFY(!model.isModified());
    for (int i = 1; i < states.size(); ++i) {
        QVERIFY(model.redo());
        QVERIFY(model.doc() == states.at(i));
    }
    QVERIFY(!model.redo());

    // A new step after undo drops what could have been redone.
    model.undo();
    QVERIFY(model.canRedo());
    model.addShape(penShape({QPointF(5, 5)}));
    QVERIFY(!model.canRedo());
}

void TestImageEdit::undoRedoLimitAndNoops()
{
    ImageEditModel model = modelOf(pattern(100, 100));
    // Changes that change nothing add no step.
    model.resetCrop();
    model.setCrop(QRect(0, 0, 100, 100));
    model.addShape(penShape({}));                                  // no points
    model.addShape(rectShape(ShapeType::Rect, QRectF(5, 5, 0, 10))); // empty
    Shape text;
    text.type   = ShapeType::Text;
    text.text   = QStringLiteral("   ");
    text.fontPx = 20;
    model.addShape(text); // only spaces
    QCOMPARE(model.undoCount(), 0);
    for (int i = 0; i < imageedit::kHistoryLimit + 20; ++i)
        model.addShape(penShape({QPointF(i % 100, 1)}));
    QCOMPARE(model.undoCount(), imageedit::kHistoryLimit);
    QCOMPARE(model.shapes().size(), imageedit::kHistoryLimit + 20);
}

void TestImageEdit::clearIsUndoable()
{
    ImageEditModel model = modelOf(pattern(100, 80));
    model.addShape(penShape({QPointF(1, 1), QPointF(30, 30)}));
    model.rotateClockwise();
    const Doc edited = model.doc();
    model.clear();
    QVERIFY(!model.isModified());
    QVERIFY(model.undo());
    QVERIFY(model.doc() == edited);
}

void TestImageEdit::setBaseKeepsOrResets()
{
    ImageEditModel model = modelOf(pattern(120, 90));
    model.addShape(penShape({QPointF(1, 1), QPointF(30, 30)}));
    model.setCrop(QRect(10, 10, 50, 40));
    const Doc doc = model.doc();
    model.dropBase();
    QVERIFY(model.base().isNull());
    QCOMPARE(model.baseSize(), QSize(120, 90));
    QCOMPARE(model.outputSize(), QSize(50, 40));
    model.setBase(pattern(120, 90)); // the same picture again: the edits and their undo steps stay
    QVERIFY(model.doc() == doc);
    QVERIFY(model.canUndo());
    model.setBase(pattern(121, 90)); // another size: starts over
    QVERIFY(!model.isModified());
    QVERIFY(!model.canUndo());
}

void TestImageEdit::viewDoesNotChangeExport()
{
    // Drawing the screen at 0.25 and at 1.0 changes nothing in the file: everything is in picture pixels.
    ImageEditModel model = modelOf(pattern(400, 300));
    model.addShape(penShape({QPointF(10, 10), QPointF(100, 80), QPointF(200, 40)}, 6));
    model.addShape(rectShape(ShapeType::Pixelate, QRectF(150, 100, 120, 80)));
    model.addShape(rectShape(ShapeType::Rect, QRectF(30, 150, 100, 60), Qt::yellow, 5));
    model.setCrop(QRect(5, 5, 380, 280));
    model.rotateClockwise();
    const QImage before = model.exportImage();
    for (qreal scale : {0.25, 1.0, 1.75}) {
        QImage screen((QSizeF(model.rotatedSize()) * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
        screen.fill(Qt::transparent);
        model.render(screen, imageedit::rotationTransform(model.rotation(), model.baseSize()) * QTransform::fromScale(scale, scale));
    }
    QCOMPARE(model.exportImage(), before);
}

void TestImageEdit::hiDpiScreenMatchesExport()
{
    // A black box drawn on a 2x screen covers the same picture area as in the file.
    ImageEditModel model = modelOf(pattern(200, 100));
    model.addShape(rectShape(ShapeType::BlackBox, QRectF(40, 20, 50, 30)));
    const QImage out = model.exportImage();
    QCOMPARE(out.pixel(40, 20), qRgb(0, 0, 0));
    QCOMPARE(out.pixel(89, 49), qRgb(0, 0, 0));
    QVERIFY(out.pixel(90, 50) != qRgb(0, 0, 0));
    QVERIFY(out.pixel(39, 19) != qRgb(0, 0, 0));
    for (qreal deviceScale : {0.5, 2.0, 1.5}) {
        QImage screen((QSizeF(200, 100) * deviceScale).toSize(), QImage::Format_ARGB32_Premultiplied);
        screen.fill(Qt::transparent);
        model.render(screen, QTransform::fromScale(deviceScale, deviceScale));
        const QRect mapped = imageedit::pixelsInside(QTransform::fromScale(deviceScale, deviceScale).mapRect(QRectF(40, 20, 50, 30)));
        QCOMPARE(screen.pixel(mapped.topLeft()), qRgb(0, 0, 0));
        QCOMPARE(screen.pixel(mapped.bottomRight()), qRgb(0, 0, 0));
        QVERIFY(screen.pixel(mapped.bottomRight() + QPoint(2, 2)) != qRgb(0, 0, 0));
    }
}

void TestImageEdit::strokeWidthFromScreen()
{
    // The editor stores logical width / scale: the same on screen at any zoom, exact in the file.
    const qreal logical = imageedit::strokeLogicalPx(1);
    QCOMPARE(logical, 6.0);
    for (qreal scale : {0.25, 1.0, 2.0}) {
        const qreal stored = logical / scale;
        QCOMPARE(stored * scale, logical);
    }
    QCOMPARE(imageedit::strokeLogicalPx(-3), imageedit::strokeLogicalPx(0)); // clamped
    QCOMPARE(imageedit::textLogicalPx(2), 44.0);
    // A thick line drawn at 0.25 zoom is 40 picture pixels wide in the file.
    ImageEditModel model = modelOf(pattern(400, 200));
    QImage         white(400, 200, QImage::Format_RGB32);
    white.fill(Qt::white);
    model.setBase(white);
    model.addShape(penShape({QPointF(20, 100), QPointF(380, 100)}, imageedit::strokeLogicalPx(2) / 0.25));
    const QImage out = model.exportImage();
    int          dark = 0;
    for (int y = 0; y < out.height(); ++y)
        dark += qBlue(out.pixel(200, y)) > 200 && qRed(out.pixel(200, y)) < 60 ? 1 : 0;
    QVERIFY2(std::abs(dark - 40) <= 2, qPrintable(QString::number(dark)));
}

void TestImageEdit::textSizeIsNotRounded()
{
    // At 8x zoom, Medium text is 3.5 picture pixels: drawn at that size, not at 4 (14% larger than typed).
    const auto inkHeight = [](qreal fontPx) {
        ImageEditModel model;
        QImage         white(60, 20, QImage::Format_RGB32);
        white.fill(Qt::white);
        model.setBase(white);
        Shape text;
        text.type   = ShapeType::Text;
        text.text   = QStringLiteral("HHH");
        text.color  = Qt::black;
        text.fontPx = fontPx;
        text.rect   = QRectF(QPointF(4, 4), QSizeF());
        model.addShape(text);
        QImage screen(480, 160, QImage::Format_ARGB32_Premultiplied);
        screen.fill(Qt::white);
        model.render(screen, QTransform::fromScale(8, 8));
        int top = -1;
        int bottom = -1;
        for (int y = 0; y < screen.height(); ++y) {
            for (int x = 0; x < screen.width(); ++x) {
                if (qRed(screen.pixel(x, y)) < 100) {
                    if (top < 0)
                        top = y;
                    bottom = y;
                    break;
                }
            }
        }
        return top < 0 ? 0 : bottom - top + 1;
    };
    const int small = inkHeight(3.5);
    const int large = inkHeight(4.0);
    QVERIFY2(small > 0 && large > 0, qPrintable(QStringLiteral("%1 %2").arg(small).arg(large)));
    const double ratio = static_cast<double>(small) / large;
    QVERIFY2(ratio > 0.80 && ratio < 0.95, qPrintable(QStringLiteral("%1 / %2").arg(small).arg(large)));
}

void TestImageEdit::shapesOrderPixelateCoversEarlier()
{
    // Hiding works on what is drawn so far: a line drawn before is pixelated too, one after stays sharp.
    QImage white(200, 100, QImage::Format_RGB32);
    white.fill(Qt::white);
    ImageEditModel model = modelOf(white);
    model.addShape(penShape({QPointF(0, 50), QPointF(200, 50)}, 2));
    model.addShape(rectShape(ShapeType::BlackBox, QRectF(0, 0, 100, 100)));
    model.addShape(penShape({QPointF(0, 20), QPointF(200, 20)}, 2));
    const QImage out = model.exportImage();
    QCOMPARE(out.pixel(50, 50), qRgb(0, 0, 0));    // the earlier line is under the box
    QVERIFY(isBlue(out.pixel(50, 20)));             // the later one is drawn over it
    QVERIFY(isBlue(out.pixel(150, 50)));            // outside the box
}

void TestImageEdit::pixelBlockRule()
{
    QCOMPARE(imageedit::pixelBlock(QRectF(0, 0, 300, 20)), 16);  // at least 16
    QCOMPARE(imageedit::pixelBlock(QRectF(0, 0, 400, 400)), 50); // min side / 8
    QCOMPARE(imageedit::pixelBlock(QRectF(0, 0, 160, 1000)), 20);
    QCOMPARE(imageedit::pixelBlock(QRectF(100, 100, -400, -400)), 50); // normalized
}

void TestImageEdit::pixelateIsBlockyQuantizedAndFixed()
{
    // Noise-like text stand-in: every block becomes one colour, a multiple of 8 within ±6.
    const QImage   base  = pattern(256, 128);
    ImageEditModel model = modelOf(base);
    const QRectF   rect(16, 16, 160, 96);
    model.addShape(rectShape(ShapeType::Pixelate, rect));
    const QImage out   = model.exportImage();
    const int    block = imageedit::pixelBlock(rect);
    QCOMPARE(block, 16);
    for (int by = 0; by < 6; ++by) {
        for (int bx = 0; bx < 10; ++bx) {
            const QPoint origin(16 + bx * block, 16 + by * block);
            const QRgb   c = out.pixel(origin);
            for (int y = 0; y < block; y += 5) {
                for (int x = 0; x < block; x += 5)
                    QCOMPARE(out.pixel(origin + QPoint(x, y)), c);
            }
            for (int channel : {qRed(c), qGreen(c), qBlue(c)}) {
                const int nearest = ((channel + 4) / 8) * 8;
                QVERIFY2(std::abs(channel - nearest) <= 6 || channel == 255 || channel == 0, qPrintable(QString::number(channel)));
            }
        }
    }
    // Outside: untouched.
    QCOMPARE(out.pixel(5, 5), base.pixel(5, 5));
    QCOMPARE(out.pixel(200, 120), base.pixel(200, 120));
    // The same every time.
    QCOMPARE(model.exportImage(), out);
    // And not just the average: the offsets differ between blocks of the same colour.
    QImage flat(128, 64, QImage::Format_RGB32);
    flat.fill(qRgb(100, 100, 100));
    ImageEditModel flatModel = modelOf(flat);
    flatModel.addShape(rectShape(ShapeType::Pixelate, QRectF(0, 0, 128, 64)));
    const QImage flatOut   = flatModel.exportImage();
    bool         different = false;
    for (int bx = 1; bx < 8; ++bx)
        different = different || flatOut.pixel(bx * 16, 0) != flatOut.pixel(0, 0);
    QVERIFY(different);
}

void TestImageEdit::pixelateSameAtAnyOffset()
{
    // The screen (a whole-pixel shift at 100%) shows exactly the blocks that go into the file.
    ImageEditModel model = modelOf(pattern(200, 120));
    model.addShape(rectShape(ShapeType::Pixelate, QRectF(30, 20, 100, 70)));
    const QImage out = model.exportImage();
    QImage       screen(260, 180, QImage::Format_ARGB32_Premultiplied);
    screen.fill(Qt::transparent);
    model.render(screen, QTransform::fromTranslate(17, 9));
    QCOMPARE(screen.copy(17, 9, 200, 120).convertToFormat(QImage::Format_RGB32), out.convertToFormat(QImage::Format_RGB32));
}

void TestImageEdit::blackBoxIsOpaque()
{
    QImage base(60, 40, QImage::Format_ARGB32);
    base.fill(Qt::transparent);
    ImageEditModel model = modelOf(base);
    model.addShape(rectShape(ShapeType::BlackBox, QRectF(10.3, 5.6, 20.2, 10.1)));
    const QImage out = model.exportImage().convertToFormat(QImage::Format_ARGB32);
    const QRect  box = imageedit::pixelsInside(QRectF(10.3, 5.6, 20.2, 10.1));
    QCOMPARE(box, QRect(10, 6, 20, 10));
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x)
            QCOMPARE(out.pixel(x, y), qRgba(0, 0, 0, 255));
    }
    QCOMPARE(qAlpha(out.pixel(9, 6)), 0);
    QCOMPARE(qAlpha(out.pixel(30, 6)), 0);
}

void TestImageEdit::fitAspectCases()
{
    const QRect photo(0, 0, 4032, 3024);
    QCOMPARE(imageedit::fitAspect(photo, 16.0 / 9.0, QRectF(photo).center()), QRect(0, 378, 4032, 2268));
    QCOMPARE(imageedit::fitAspect(photo, 1.0, QRectF(photo).center()), QRect(504, 0, 3024, 3024));
    QCOMPARE(imageedit::fitAspect(photo, 4.0 / 3.0, QRectF(photo).center()), photo);
    // Near an edge it stays inside.
    QCOMPARE(imageedit::fitAspect(photo, 1.0, QPointF(10, 10)), QRect(0, 0, 3024, 3024));
    QCOMPARE(imageedit::fitAspect(photo, 1.0, QPointF(4000, 10)), QRect(1008, 0, 3024, 3024));
    // Portrait shapes in a landscape picture.
    const QRect tall = imageedit::fitAspect(photo, 9.0 / 16.0, QRectF(photo).center());
    QCOMPARE(tall.height(), 3024);
    QCOMPARE(tall.width(), 1701);
    QCOMPARE(imageedit::fitAspect(photo, 0, QPointF()), photo); // free
}

void TestImageEdit::dragCropFree()
{
    const QRect bounds(0, 0, 1000, 800);
    const QRect start(100, 100, 400, 300);
    QCOMPARE(imageedit::dragCrop(start, Handle::Move, QPointF(50, -20), bounds, 0, 16), QRect(150, 80, 400, 300));
    QCOMPARE(imageedit::dragCrop(start, Handle::Move, QPointF(-500, 900), bounds, 0, 16), QRect(0, 500, 400, 300)); // stays inside
    QCOMPARE(imageedit::dragCrop(start, Handle::BottomRight, QPointF(100, 50), bounds, 0, 16), QRect(100, 100, 500, 350));
    QCOMPARE(imageedit::dragCrop(start, Handle::TopLeft, QPointF(-200, -200), bounds, 0, 16), QRect(0, 0, 500, 400)); // clamped
    QCOMPARE(imageedit::dragCrop(start, Handle::Left, QPointF(1000, 0), bounds, 0, 16), QRect(484, 100, 16, 300));   // minimum, no flip
    QCOMPARE(imageedit::dragCrop(start, Handle::Bottom, QPointF(0, 10000), bounds, 0, 16), QRect(100, 100, 400, 700));
    QCOMPARE(imageedit::dragCrop(start, Handle::None, QPointF(10, 10), bounds, 0, 16), start);
}

void TestImageEdit::dragCropAspect()
{
    const QRect  bounds(0, 0, 1600, 900);
    const QRect  start(200, 100, 640, 360); // 16:9
    const double aspect = 16.0 / 9.0;
    const auto   keeps  = [aspect](const QRect& r) { return std::abs(r.width() - r.height() * aspect) <= 2.0; };

    const QRect corner = imageedit::dragCrop(start, Handle::BottomRight, QPointF(160, 10), bounds, aspect, 16);
    QVERIFY(keeps(corner));
    QCOMPARE(corner.topLeft(), start.topLeft()); // the opposite corner stays
    QCOMPARE(corner.width(), 800);
    const QRect big = imageedit::dragCrop(start, Handle::BottomRight, QPointF(5000, 5000), bounds, aspect, 16);
    QVERIFY(keeps(big));
    QVERIFY(bounds.contains(big));
    const QRect side = imageedit::dragCrop(start, Handle::Right, QPointF(320, 0), bounds, aspect, 16);
    QVERIFY(keeps(side));
    QCOMPARE(side.left(), start.left());
    QVERIFY(bounds.contains(side));
    const QRect top = imageedit::dragCrop(start, Handle::Top, QPointF(0, -100), bounds, aspect, 16);
    QVERIFY(keeps(top));
    QCOMPARE(top.bottom(), start.bottom());
    QVERIFY(bounds.contains(top));
    const QRect small = imageedit::dragCrop(start, Handle::TopLeft, QPointF(2000, 2000), bounds, aspect, 16);
    QVERIFY(small.height() >= 16 && small.width() >= 16);
    QVERIFY(keeps(small));
    const QRect square = imageedit::dragCrop(QRect(0, 0, 100, 100), Handle::BottomLeft, QPointF(-30, 300), bounds, 1.0, 16);
    QCOMPARE(square.width(), square.height());
    QVERIFY(bounds.contains(square));
}

void TestImageEdit::snapAndSquare()
{
    const QPointF from(10, 10);
    const QPointF snapped = imageedit::snap45(from, QPointF(110, 20)); // nearly horizontal: the pointer's x stays
    QVERIFY(std::abs(snapped.y() - 10.0) < 1e-9);
    QVERIFY(std::abs(snapped.x() - 110.0) < 1e-9);
    const QPointF diagonal = imageedit::snap45(from, QPointF(60, 66));
    QVERIFY(std::abs((diagonal.x() - 10) - (diagonal.y() - 10)) < 1e-9);
    QCOMPARE(imageedit::snap45(from, from), from);
    QCOMPARE(imageedit::squareFrom(QPointF(10, 10), QPointF(40, 20)), QRectF(10, 10, 30, 30));
    QCOMPARE(imageedit::squareFrom(QPointF(10, 10), QPointF(-10, 0)), QRectF(-10, -10, 20, 20));
}

void TestImageEdit::transparency()
{
    QVERIFY(!imageedit::hasTransparency(QImage()));
    QVERIFY(!imageedit::hasTransparency(pattern(10, 10)));
    QImage argb(10, 10, QImage::Format_ARGB32);
    argb.fill(qRgba(1, 2, 3, 255));
    QVERIFY(!imageedit::hasTransparency(argb)); // an alpha channel, but all opaque
    argb.setPixel(9, 9, qRgba(0, 0, 0, 128));
    QVERIFY(imageedit::hasTransparency(argb));
}

void TestImageEdit::encodeFormats()
{
    const QImage photo = pattern(64, 48);
    const auto   format = [](const imageedit::Encoded& e) {
        QBuffer buffer;
        buffer.setData(e.data);
        buffer.open(QIODevice::ReadOnly);
        return QImageReader(&buffer).format();
    };
    // A screenshot stays PNG; a photo stays JPEG; WebP becomes JPEG.
    imageedit::Encoded png = imageedit::encode(photo, QStringLiteral("shot.png"), false, true);
    QCOMPARE(png.extension, QStringLiteral("png"));
    QCOMPARE(format(png), QByteArray("png"));
    imageedit::Encoded jpeg = imageedit::encode(photo, QStringLiteral("holiday.JPEG"), false, true);
    QCOMPARE(jpeg.extension, QStringLiteral("jpeg"));
    QCOMPARE(format(jpeg), QByteArray("jpeg"));
    QCOMPARE(imageedit::encode(photo, QStringLiteral("a.jfif"), false, true).extension, QStringLiteral("jfif"));
    QCOMPARE(imageedit::encode(photo, QStringLiteral("a.webp"), false, true).extension, QStringLiteral("jpg"));
    QCOMPARE(imageedit::encode(photo, QStringLiteral("a.bmp"), false, true).extension, QStringLiteral("png"));
    QCOMPARE(imageedit::encode(photo, QStringLiteral("a.jpg"), true, true).extension, QStringLiteral("png")); // a paste
    // Transparency keeps PNG, even from a JPEG name.
    QImage clear(32, 32, QImage::Format_ARGB32);
    clear.fill(Qt::transparent);
    QCOMPARE(imageedit::encode(clear, QStringLiteral("odd.jpg"), false, true).extension, QStringLiteral("png"));
    // The paste rule: a PNG over 2 MB without transparency becomes JPEG (when the setting is on).
    QImage noise(1400, 1000, QImage::Format_RGB32);
    quint32 seed = 12345;
    for (int y = 0; y < noise.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(noise.scanLine(y));
        for (int x = 0; x < noise.width(); ++x) {
            seed    = seed * 1664525U + 1013904223U;
            line[x] = 0xff000000U | (seed >> 8);
        }
    }
    const imageedit::Encoded large = imageedit::encode(noise, QStringLiteral("big.png"), false, true);
    QCOMPARE(large.extension, QStringLiteral("jpg"));
    QCOMPARE(format(large), QByteArray("jpeg"));
    QCOMPARE(imageedit::encode(noise, QStringLiteral("big.png"), false, false).extension, QStringLiteral("png"));
    QVERIFY(imageedit::encode(QImage(), QStringLiteral("x.png"), false, true).data.isEmpty());
}

void TestImageEdit::exportNames()
{
    QCOMPARE(imageedit::exportName(QStringLiteral("holiday.jpg"), QStringLiteral("jpg")), QStringLiteral("holiday.jpg"));
    QCOMPARE(imageedit::exportName(QStringLiteral("holiday.JPG"), QStringLiteral("jpg")), QStringLiteral("holiday.JPG"));
    QCOMPARE(imageedit::exportName(QStringLiteral("holiday.jpeg"), QStringLiteral("jpg")), QStringLiteral("holiday.jpeg"));
    QCOMPARE(imageedit::exportName(QStringLiteral("shot.png"), QStringLiteral("jpg")), QStringLiteral("shot.jpg"));
    QCOMPARE(imageedit::exportName(QStringLiteral("scan.bmp"), QStringLiteral("png")), QStringLiteral("scan.png"));
    QCOMPARE(imageedit::exportName(QStringLiteral("my.trip.webp"), QStringLiteral("jpg")), QStringLiteral("my.trip.jpg"));
    QCOMPARE(imageedit::exportName(QStringLiteral(".png"), QStringLiteral("png")), QStringLiteral("image.png"));
    QCOMPARE(imageedit::exportName(QString::fromUtf8("عکس.png"), QStringLiteral("jpg")), QString::fromUtf8("عکس.jpg"));
}

void TestImageEdit::writeEditedFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const imageedit::Encoded encoded = imageedit::encode(pattern(20, 20), QStringLiteral("a.png"), false, true);
    const QString            path    = imageedit::writeEdited(encoded, dir.path() + QStringLiteral("/one"), QStringLiteral("a.png"));
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("a.png"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), encoded.data);
    file.close();
    // The folder is a file: nothing written, nothing left.
    const QString blocker = dir.path() + QStringLiteral("/blocked");
    QFile         block(blocker);
    QVERIFY(block.open(QIODevice::WriteOnly));
    block.close();
    QVERIFY(imageedit::writeEdited(encoded, blocker, QStringLiteral("a.png")).isEmpty());
    QVERIFY(imageedit::writeEdited(imageedit::Encoded(), dir.path(), QStringLiteral("a.png")).isEmpty());
}

void TestImageEdit::exifOrientationAndStripping()
{
    // Stored 40 x 20, left half red, right half blue, EXIF "rotate 90° clockwise".
    QImage stored(40, 20, QImage::Format_RGB32);
    stored.fill(Qt::blue);
    QPainter(&stored).fillRect(0, 0, 20, 20, Qt::red);
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/phone.jpg");
    QFile         file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(jpegWithOrientation6(stored));
    file.close();

    imageedit::OpenProblem problem = imageedit::OpenProblem::Unreadable;
    const QImage           upright = imageedit::loadForEditing(path, &problem);
    QCOMPARE(int(problem), int(imageedit::OpenProblem::None));
    QCOMPARE(upright.size(), QSize(20, 40)); // as shown: turned
    QVERIFY(isRed(upright.pixel(10, 5)));
    QVERIFY(isBlue(upright.pixel(10, 35)));

    // Exported upright, without the EXIF block (no orientation, no location).
    ImageEditModel model = modelOf(upright);
    model.addShape(penShape({QPointF(2, 20), QPointF(18, 20)}, 2));
    const imageedit::Encoded encoded = imageedit::encode(model.exportImage(), QStringLiteral("phone.jpg"), false, true);
    QCOMPARE(encoded.extension, QStringLiteral("jpg"));
    QVERIFY(!encoded.data.contains(QByteArray("Exif\0\0", 6)));
    QBuffer buffer;
    buffer.setData(encoded.data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QImage reread = reader.read();
    QCOMPARE(reread.size(), QSize(20, 40));
    QVERIFY(isRed(reread.pixel(10, 5)));
    QVERIFY(isBlue(reread.pixel(10, 35)));
}

void TestImageEdit::openProblems()
{
    QTemporaryDir dir;
    const auto    write = [&dir](const QString& name, const QByteArray& data) {
        const QString path = dir.path() + QLatin1Char('/') + name;
        QFile         file(path);
        file.open(QIODevice::WriteOnly);
        file.write(data);
        return path;
    };
    imageedit::OpenProblem problem = imageedit::OpenProblem::None;
    QVERIFY(imageedit::loadForEditing(write(QStringLiteral("notes.png"), QByteArray("not a picture")), &problem).isNull());
    QCOMPARE(int(problem), int(imageedit::OpenProblem::Unreadable));
    QVERIFY(imageedit::loadForEditing(dir.path() + QStringLiteral("/missing.png"), &problem).isNull());
    QCOMPARE(int(problem), int(imageedit::OpenProblem::Unreadable));
    QVERIFY(imageedit::loadForEditing(write(QStringLiteral("anim.gif"), twoFrameGif()), &problem).isNull());
    QCOMPARE(int(problem), int(imageedit::OpenProblem::Animated));
    // Too large is known from the header alone (nothing is decoded).
    QVERIFY(imageedit::loadForEditing(write(QStringLiteral("huge.png"), pngHeaderOnly(9000, 9000)), &problem).isNull());
    QCOMPARE(int(problem), int(imageedit::OpenProblem::TooLarge));
    QImage small = pattern(30, 20);
    small.save(dir.path() + QStringLiteral("/ok.png"));
    const QImage ok = imageedit::loadForEditing(dir.path() + QStringLiteral("/ok.png"), &problem);
    QCOMPARE(int(problem), int(imageedit::OpenProblem::None));
    QCOMPARE(ok.size(), QSize(30, 20));
    QVERIFY(!imageedit::openProblemText(imageedit::OpenProblem::TooLarge).isEmpty());
    QVERIFY(imageedit::openProblemText(imageedit::OpenProblem::None).isEmpty());
    QVERIFY(imageedit::openProblemText(imageedit::OpenProblem::TooLarge).contains(QString::number(imageedit::kMaxPixels / 1000000)));
}

void TestImageEdit::palette()
{
    QCOMPARE(imageedit::paletteColor(0), QColor(0xF2, 0x3F, 0x43));
    QCOMPARE(imageedit::paletteColor(3), QColor(0x58, 0x65, 0xF2));
    QCOMPARE(imageedit::paletteColor(5), QColor(0, 0, 0));
    QCOMPARE(imageedit::paletteColor(99), imageedit::paletteColor(5));
    QCOMPARE(imageedit::paletteColorName(1), QStringLiteral("Yellow"));
    imageedit::Prefs prefs;
    prefs.color    = 9;
    prefs.stroke   = -1;
    prefs.hideMode = 4;
    const imageedit::Prefs clamped = imageedit::clamped(prefs);
    QCOMPARE(clamped.color, 5);
    QCOMPARE(clamped.stroke, 0);
    QCOMPARE(clamped.hideMode, 1);
}

TSMEDIA_REGISTER_TEST(TestImageEdit)

#include "tst_imageedit.moc"
