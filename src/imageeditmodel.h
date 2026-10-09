#pragma once

// 2.2 editor: the crop & annotate editor without any widget (QtCore/QtGui only, unit-tested): the
// picture, its rotation and crop, the shapes drawn on it, undo/redo, drawing all of that at any scale,
// and the edited file it exports. ImageEditor (imageeditor.h) is the window around it.
//
// Coordinates: everything is stored in pixels of the picture as loaded, with its EXIF orientation
// already applied ("base" pixels). Zoom, the screen's DPI and later rotations therefore never change
// what was drawn, and the export is drawn at full resolution from the same numbers the screen used.
// The output is the base picture rotated clockwise by rotation(), cut to crop() (also base pixels).

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>

class QPainter;
class QPainterPath;

namespace imageedit {

// Pictures up to this many pixels can be edited (8K UHD; 16 MP in the 32-bit client, whose address
// space is small: the picture, the display copies and the export each need 4 bytes per pixel).
constexpr qint64 kMaxPixels    = sizeof(void*) >= 8 ? 7680LL * 4320 : 4096LL * 4096;
constexpr int    kHistoryLimit = 100; // undo steps
constexpr int    kMinCropSide  = 16;  // base pixels (or the picture's side, if smaller)

// ---- palette and sizes (the editor's options bar) -------------------------------------------------

constexpr int kColorCount  = 6; // keys 1-6
constexpr int kStrokeCount = 3; // Thin, Medium, Thick ([ and ])

QColor  paletteColor(int index);     // Red #F23F43, Yellow #F0B232, Green #23A55A, Blue #5865F2, White, Black
QString paletteColorName(int index); // "Red"
QString strokeName(int index);       // "Thin", "Medium", "Thick"
qreal   strokeLogicalPx(int index);  // line width on screen: 3, 6, 10
qreal   textLogicalPx(int index);    // text size on screen: 18, 28, 44

// The editor's last choices, kept in Settings (editorColor, editorStroke, editorHideMode).
struct Prefs {
    int color    = 0; // Red
    int stroke   = 1; // Medium
    int hideMode = 0; // 0: Pixelate, 1: Black box
};
Prefs clamped(const Prefs& prefs);

// ---- the document ---------------------------------------------------------------------------------

enum class ShapeType { Pen, Arrow, Rect, Text, Pixelate, BlackBox };

struct Shape {
    ShapeType        type = ShapeType::Pen;
    QColor           color;
    qreal            width = 0;        // pen, arrow, rectangle: line width
    QVector<QPointF> points;           // pen: the stroke; arrow: tail, head
    QRectF           rect;             // rectangle, pixelate, black box (normalized); text: top left corner
    QString          text;             // one line
    qreal            fontPx       = 0; // text size
    int              textRotation = 0; // the picture's rotation when the text was written: it reads upright then

    bool operator==(const Shape& other) const;
    bool operator!=(const Shape& other) const { return !(*this == other); }
};

struct Doc {
    int            rotation = 0; // 0, 90, 180 or 270, clockwise
    QRect          crop;         // base pixels; empty: the whole picture
    QVector<Shape> shapes;       // drawn in this order

    bool operator==(const Doc& other) const;
    bool operator!=(const Doc& other) const { return !(*this == other); }
};

class ImageEditModel
{
  public:
    // The picture to edit (any format; kept as RGB32, or ARGB32_Premultiplied when it has an alpha
    // channel). The document and its history stay when the size is the same as before (reopening an
    // edited picture); otherwise they start over.
    void          setBase(const QImage& image);
    const QImage& base() const { return m_base; }
    QSize         baseSize() const { return m_baseSize; } // also after dropBase()
    // Frees the picture's memory; the document and its history stay (setBase() brings it back).
    void          dropBase() { m_base = QImage(); }

    const Doc&            doc() const { return m_doc; }
    int                   rotation() const { return m_doc.rotation; }
    QRect                 crop() const; // base pixels; the whole picture when not cropped
    bool                  isCropped() const { return crop() != QRect(QPoint(0, 0), m_baseSize); }
    const QVector<Shape>& shapes() const { return m_doc.shapes; }
    QSize                 rotatedSize() const;  // the whole picture after the rotation
    QRect                 cropInRotated() const; // the crop in rotated-picture pixels
    QSize                 outputSize() const;   // what the export measures
    bool                  isModified() const { return m_doc != Doc(); } // differs from the untouched picture

    // Each change is one undo step; a change that changes nothing adds no step.
    void addShape(const Shape& shape);
    void rotateClockwise();
    void setCrop(const QRect& baseRect); // clamped to the picture; at least kMinCropSide
    void resetCrop();
    void clear(); // back to the untouched picture (undoable)

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    int  undoCount() const { return m_undo.size(); }
    bool undo();
    bool redo();

    // base pixels -> output pixels (rotation, then the crop's offset).
    QTransform baseToOutput() const;

    // Draws the edited picture into target (RGB32 or ARGB32_Premultiplied; others are converted) with
    // baseToTarget (a scale, a rotation by a multiple of 90° and a translation). scaledBase: a smaller
    // copy of base() to draw from instead (the screen, zoomed out), shapeCount: only the first ones
    // (-1: all).
    void render(QImage& target, const QTransform& baseToTarget, const QImage* scaledBase = nullptr, int shapeCount = -1) const;

    // The edited picture at full resolution: crop size, rotated, never scaled. RGB32, or
    // ARGB32_Premultiplied when the picture has an alpha channel. Fresh pixels: no metadata.
    QImage exportImage() const;

  private:
    void push(const Doc& next);

    QImage       m_base;
    QSize        m_baseSize;
    Doc          m_doc;
    QVector<Doc> m_undo; // the states before each step, oldest first
    QVector<Doc> m_redo;
};

// ---- geometry ----------------------------------------------------------------------------------------

// base pixels -> rotated-picture pixels for a picture of baseSize turned clockwise by rotation.
QTransform rotationTransform(int rotation, const QSize& baseSize);
QRect      baseToRotated(const QRect& rect, int rotation, const QSize& baseSize);
QRect      rotatedToBase(const QRect& rect, int rotation, const QSize& baseSize);

// A rectangle in device pixels -> the whole pixels whose centres lie inside it.
QRect pixelsInside(const QRectF& rect);

// ---- drawing --------------------------------------------------------------------------------------------

// Pen, arrow, rectangle and text, in base pixels (the painter's transform maps them to the target).
void         drawShape(QPainter& painter, const Shape& shape);
QPainterPath penPath(const QVector<QPointF>& points); // smoothed through the points' midpoints
// Shift while drawing: arrows and pen lines snap to 45°, rectangles become squares.
QPointF      snap45(const QPointF& from, const QPointF& to);
QRectF       squareFrom(const QPointF& from, const QPointF& to);

// The pixelate block for a region: max(16, round(min(w, h) / 8)) base pixels.
int  pixelBlock(const QRectF& rect);
// Replaces each block of rect (base pixels) in target with its average colour, quantized to 32 levels
// plus a small fixed per-block offset (from seed), so the text below can't be recovered by trying.
void pixelate(QImage& target, const QTransform& baseToTarget, const QRectF& rect, quint32 seed);
// Solid #000000, fully opaque.
void blackBox(QImage& target, const QTransform& baseToTarget, const QRectF& rect);

// ---- crop helpers (rotated-picture pixels: the aspect is what the viewer sees) ---------------------

enum class Handle { None, Move, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

// The largest rectangle of aspect (width / height) inside bounds, centred on center as far as it can be.
QRect fitAspect(const QRect& bounds, double aspect, const QPointF& center);
// start dragged by delta at handle, kept inside bounds, at least minSide, and of aspect (0: any).
QRect dragCrop(const QRect& start, Handle handle, const QPointF& delta, const QRect& bounds, double aspect, int minSide);

// ---- opening and saving ------------------------------------------------------------------------------

enum class OpenProblem { None, Unreadable, Animated, TooLarge };
QString openProblemText(OpenProblem problem); // "Animated images can't be edited", ...

// Reads a picture for editing (EXIF orientation applied; the size is checked before decoding).
QImage loadForEditing(const QString& path, OpenProblem* problem);

bool hasTransparency(const QImage& image); // an alpha channel with at least one pixel not fully opaque

struct Encoded {
    QByteArray data;
    QString    extension; // "png", "jpg" (or the source's own "jpeg"/"jfif")
};
// The edited picture as a file: PNG for PNG/BMP sources, pastes and anything with transparency; JPEG
// (quality 92) for JPEG and WebP photos. Then the paste rule: a PNG over 2 MB without transparency
// becomes JPEG (quality 90) when convertLargePngToJpeg is on. No metadata is written (no EXIF).
Encoded encode(const QImage& image, const QString& sourceName, bool pasted, bool convertLargePngToJpeg);
// "holiday.jpg" + "png" -> "holiday.png"; the source's own extension is kept when the format is.
QString exportName(const QString& sourceName, const QString& extension);
// Writes encoded as dir/<exportName>; returns the path, or an empty string (and nothing left behind).
QString writeEdited(const Encoded& encoded, const QString& dir, const QString& sourceName);

} // namespace imageedit
