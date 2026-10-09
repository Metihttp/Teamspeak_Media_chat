#include "imageeditmodel.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

#include "i18n.h"

namespace imageedit {

namespace {

constexpr qint64 kLargePngBytes = 2 * 1024 * 1024; // the paste rule (Core::uploadImage)
constexpr int    kPhotoQuality  = 92;              // JPEG and WebP photos
constexpr int    kPngToJpegQuality = 90;           // the paste rule

QImage::Format formatFor(const QImage& image)
{
    return image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
}

void ensureRenderFormat(QImage& image)
{
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// A fixed pseudo-random number for a block: the same picture always pixelates the same way.
quint32 mix(quint32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

int blockNoise(quint32 seed, int column, int row, int channel)
{
    const quint32 h = mix(seed ^ mix(static_cast<quint32>(column) * 0x9E3779B1U + static_cast<quint32>(row) * 0x85EBCA77U + static_cast<quint32>(channel)));
    return static_cast<int>(h % 13U) - 6; // -6..6
}

// 32 levels (multiples of 8) plus the block's offset.
int quantize(int value, int noise, int limit)
{
    const int level = ((value + 4) / 8) * 8;
    return qBound(0, level + noise, limit);
}

double luminance(const QColor& color)
{
    const auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

// Black around light colours, white around dark ones: whichever stands out more.
QColor outlineFor(const QColor& color)
{
    const double l          = luminance(color);
    const double blackRatio = (l + 0.05) / 0.05;
    const double whiteRatio = 1.05 / (l + 0.05);
    return blackRatio >= whiteRatio ? QColor(0, 0, 0) : QColor(255, 255, 255);
}

bool isJpegSuffix(const QString& suffix)
{
    return suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("jfif");
}

} // namespace

// ---- palette ----------------------------------------------------------------------------------------

QColor paletteColor(int index)
{
    static const QRgb colors[kColorCount] = {0xF23F43, 0xF0B232, 0x23A55A, 0x5865F2, 0xFFFFFF, 0x000000};
    return QColor(colors[qBound(0, index, kColorCount - 1)]);
}

QString paletteColorName(int index)
{
    switch (qBound(0, index, kColorCount - 1)) {
    case 0:
        return i18n::t("Red");
    case 1:
        return i18n::t("Yellow");
    case 2:
        return i18n::t("Green");
    case 3:
        return i18n::t("Blue");
    case 4:
        return i18n::t("White");
    default:
        return i18n::t("Black");
    }
}

QString strokeName(int index)
{
    switch (qBound(0, index, kStrokeCount - 1)) {
    case 0:
        return i18n::t("Thin");
    case 1:
        return i18n::t("Medium");
    default:
        return i18n::t("Thick");
    }
}

qreal strokeLogicalPx(int index)
{
    static const qreal widths[kStrokeCount] = {3.0, 6.0, 10.0};
    return widths[qBound(0, index, kStrokeCount - 1)];
}

qreal textLogicalPx(int index)
{
    static const qreal sizes[kStrokeCount] = {18.0, 28.0, 44.0};
    return sizes[qBound(0, index, kStrokeCount - 1)];
}

Prefs clamped(const Prefs& prefs)
{
    Prefs out;
    out.color    = qBound(0, prefs.color, kColorCount - 1);
    out.stroke   = qBound(0, prefs.stroke, kStrokeCount - 1);
    out.hideMode = qBound(0, prefs.hideMode, 1);
    return out;
}

// ---- the document ---------------------------------------------------------------------------------

namespace {

// Element by element: QVector's own operator== uses MSVC's deprecated checked iterators (warning STL4043).
template <typename T>
bool sameElements(const QVector<T>& a, const QVector<T>& b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (!(a.at(i) == b.at(i)))
            return false;
    }
    return true;
}

} // namespace

bool Shape::operator==(const Shape& other) const
{
    return type == other.type && color == other.color && width == other.width && sameElements(points, other.points) && rect == other.rect
           && text == other.text && fontPx == other.fontPx && textRotation == other.textRotation;
}

bool Doc::operator==(const Doc& other) const
{
    return rotation == other.rotation && crop == other.crop && sameElements(shapes, other.shapes);
}

void ImageEditModel::setBase(const QImage& image)
{
    if (image.size() != m_baseSize) {
        m_doc = Doc();
        m_undo.clear();
        m_redo.clear();
    }
    m_baseSize = image.size();
    m_base     = image.isNull() ? QImage() : (image.format() == formatFor(image) ? image : image.convertToFormat(formatFor(image)));
}

QRect ImageEditModel::crop() const
{
    return m_doc.crop.isEmpty() ? QRect(QPoint(0, 0), m_baseSize) : m_doc.crop;
}

QSize ImageEditModel::rotatedSize() const
{
    return m_doc.rotation % 180 ? m_baseSize.transposed() : m_baseSize;
}

QRect ImageEditModel::cropInRotated() const
{
    return baseToRotated(crop(), m_doc.rotation, m_baseSize);
}

QSize ImageEditModel::outputSize() const
{
    return cropInRotated().size();
}

void ImageEditModel::push(const Doc& next)
{
    if (next == m_doc)
        return;
    m_undo.append(m_doc);
    while (m_undo.size() > kHistoryLimit)
        m_undo.removeFirst();
    m_redo.clear();
    m_doc = next;
}

void ImageEditModel::addShape(const Shape& shape)
{
    switch (shape.type) {
    case ShapeType::Pen:
    case ShapeType::Arrow:
        if (shape.points.isEmpty() || (shape.type == ShapeType::Arrow && shape.points.size() < 2) || shape.width <= 0)
            return;
        break;
    case ShapeType::Rect:
    case ShapeType::Pixelate:
    case ShapeType::BlackBox:
        if (shape.rect.normalized().isEmpty())
            return;
        break;
    case ShapeType::Text:
        if (shape.text.trimmed().isEmpty() || shape.fontPx <= 0)
            return;
        break;
    }
    Doc next = m_doc;
    Shape s  = shape;
    if (s.type != ShapeType::Text)
        s.rect = s.rect.normalized();
    next.shapes.append(s);
    push(next);
}

void ImageEditModel::rotateClockwise()
{
    Doc next      = m_doc;
    next.rotation = (m_doc.rotation + 90) % 360;
    push(next);
}

void ImageEditModel::setCrop(const QRect& baseRect)
{
    const QRect whole(QPoint(0, 0), m_baseSize);
    QRect       r = baseRect.normalized() & whole;
    if (r.isEmpty())
        return;
    // At least kMinCropSide (or the whole side of a tiny picture), grown inside the picture.
    const int minW = qMin(kMinCropSide, whole.width());
    const int minH = qMin(kMinCropSide, whole.height());
    if (r.width() < minW)
        r = QRect(qBound(0, r.left(), whole.width() - minW), r.top(), minW, r.height());
    if (r.height() < minH)
        r = QRect(r.left(), qBound(0, r.top(), whole.height() - minH), r.width(), minH);
    Doc next  = m_doc;
    next.crop = r == whole ? QRect() : r;
    push(next);
}

void ImageEditModel::resetCrop()
{
    Doc next  = m_doc;
    next.crop = QRect();
    push(next);
}

void ImageEditModel::clear()
{
    push(Doc());
}

bool ImageEditModel::undo()
{
    if (m_undo.isEmpty())
        return false;
    m_redo.append(m_doc);
    m_doc = m_undo.takeLast();
    return true;
}

bool ImageEditModel::redo()
{
    if (m_redo.isEmpty())
        return false;
    m_undo.append(m_doc);
    m_doc = m_redo.takeLast();
    return true;
}

QTransform ImageEditModel::baseToOutput() const
{
    const QRect c = cropInRotated();
    return rotationTransform(m_doc.rotation, m_baseSize) * QTransform::fromTranslate(-c.x(), -c.y());
}

void ImageEditModel::render(QImage& target, const QTransform& baseToTarget, const QImage* scaledBase, int shapeCount) const
{
    if (target.isNull())
        return;
    ensureRenderFormat(target);
    const QImage& source = scaledBase && !scaledBase->isNull() ? *scaledBase : m_base;
    const int     count  = shapeCount < 0 ? m_doc.shapes.size() : qMin(shapeCount, m_doc.shapes.size());

    QPainter p(&target);
    if (!source.isNull() && !m_baseSize.isEmpty()) {
        const QTransform sourceToTarget = QTransform::fromScale(qreal(m_baseSize.width()) / source.width(), qreal(m_baseSize.height()) / source.height()) * baseToTarget;
        // A whole-pixel shift copies the pixels as they are (the export); anything else is filtered.
        const bool exact = sourceToTarget.type() <= QTransform::TxTranslate && sourceToTarget.dx() == std::floor(sourceToTarget.dx())
                           && sourceToTarget.dy() == std::floor(sourceToTarget.dy());
        p.setRenderHint(QPainter::SmoothPixmapTransform, !exact);
        p.setTransform(sourceToTarget);
        p.drawImage(QPointF(0, 0), source);
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(baseToTarget);
    for (int i = 0; i < count; ++i) {
        const Shape& shape = m_doc.shapes.at(i);
        if (shape.type == ShapeType::Pixelate || shape.type == ShapeType::BlackBox) {
            // Works on what is drawn so far (the shapes before it included).
            p.end();
            if (shape.type == ShapeType::Pixelate)
                pixelate(target, baseToTarget, shape.rect, 0x9E3779B9U * static_cast<quint32>(i + 1));
            else
                blackBox(target, baseToTarget, shape.rect);
            p.begin(&target);
            p.setRenderHint(QPainter::Antialiasing);
            p.setTransform(baseToTarget);
            continue;
        }
        drawShape(p, shape);
    }
}

QImage ImageEditModel::exportImage() const
{
    if (m_base.isNull())
        return {};
    const QRect c = crop();
    QImage      out(c.size(), formatFor(m_base));
    if (out.isNull())
        return {}; // out of memory
    out.fill(Qt::transparent);
    render(out, QTransform::fromTranslate(-c.x(), -c.y()));
    if (m_doc.rotation)
        out = out.transformed(QTransform().rotate(m_doc.rotation)); // exact for multiples of 90°
    return out;
}

// ---- geometry ----------------------------------------------------------------------------------------

QTransform rotationTransform(int rotation, const QSize& baseSize)
{
    const qreal w = baseSize.width();
    const qreal h = baseSize.height();
    switch (((rotation % 360) + 360) % 360) {
    case 90:
        return QTransform(0, 1, -1, 0, h, 0); // (x, y) -> (h - y, x)
    case 180:
        return QTransform(-1, 0, 0, -1, w, h);
    case 270:
        return QTransform(0, -1, 1, 0, 0, w); // (x, y) -> (y, w - x)
    default:
        return QTransform();
    }
}

namespace {

QRect mappedRect(const QTransform& t, const QRect& rect)
{
    const QRectF r = t.mapRect(QRectF(rect));
    const int    x = qRound(r.left());
    const int    y = qRound(r.top());
    return QRect(x, y, qRound(r.right()) - x, qRound(r.bottom()) - y);
}

} // namespace

QRect baseToRotated(const QRect& rect, int rotation, const QSize& baseSize)
{
    return mappedRect(rotationTransform(rotation, baseSize), rect);
}

QRect rotatedToBase(const QRect& rect, int rotation, const QSize& baseSize)
{
    return mappedRect(rotationTransform(rotation, baseSize).inverted(), rect);
}

QRect pixelsInside(const QRectF& rect)
{
    const QRectF r  = rect.normalized();
    const int    x0 = static_cast<int>(std::ceil(r.left() - 0.5));
    const int    y0 = static_cast<int>(std::ceil(r.top() - 0.5));
    const int    x1 = static_cast<int>(std::ceil(r.right() - 0.5));
    const int    y1 = static_cast<int>(std::ceil(r.bottom() - 0.5));
    return QRect(x0, y0, qMax(0, x1 - x0), qMax(0, y1 - y0));
}

// ---- drawing --------------------------------------------------------------------------------------------

QPainterPath penPath(const QVector<QPointF>& points)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points.first());
    if (points.size() == 2) {
        path.lineTo(points.last());
        return path;
    }
    for (int i = 1; i + 1 < points.size(); ++i)
        path.quadTo(points.at(i), (points.at(i) + points.at(i + 1)) / 2.0);
    if (points.size() > 1)
        path.lineTo(points.last());
    return path;
}

QPointF snap45(const QPointF& from, const QPointF& to)
{
    const QPointF d = to - from;
    const qreal   length = std::hypot(d.x(), d.y());
    if (length <= 0)
        return to;
    const qreal step    = 3.14159265358979323846 / 4.0;
    const qreal angle   = std::atan2(d.y(), d.x());
    const qreal snapped = std::round(angle / step) * step;
    const qreal along   = length * std::cos(angle - snapped); // the cursor's distance along the snapped line
    return from + QPointF(std::cos(snapped), std::sin(snapped)) * along;
}

QRectF squareFrom(const QPointF& from, const QPointF& to)
{
    const QPointF d    = to - from;
    const qreal   side = qMax(std::abs(d.x()), std::abs(d.y()));
    return QRectF(from, QSizeF(d.x() < 0 ? -side : side, d.y() < 0 ? -side : side)).normalized();
}

void drawShape(QPainter& p, const Shape& shape)
{
    p.save();
    switch (shape.type) {
    case ShapeType::Pen: {
        if (shape.points.size() == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(shape.color);
            p.drawEllipse(shape.points.first(), shape.width / 2.0, shape.width / 2.0);
            break;
        }
        p.setPen(QPen(shape.color, shape.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(penPath(shape.points));
        break;
    }
    case ShapeType::Arrow: {
        if (shape.points.size() < 2)
            break;
        const QPointF tail   = shape.points.at(0);
        const QPointF head   = shape.points.at(1);
        const QPointF d      = head - tail;
        const qreal   length = std::hypot(d.x(), d.y());
        if (length <= 0.01)
            break;
        const QPointF dir(d / length);
        const QPointF normal(-dir.y(), dir.x());
        qreal         headLength = shape.width * 4.5;
        qreal         headHalf   = shape.width * 2.4;
        if (headLength > length) {
            headHalf   = headHalf * length / headLength;
            headLength = length;
        }
        const QPointF back = head - dir * headLength;
        p.setPen(QPen(shape.color, shape.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (length - headLength * 0.6 > 0.01)
            p.drawLine(tail, head - dir * (headLength * 0.6)); // its round end hides under the head
        QPolygonF triangle;
        triangle << head << back + normal * headHalf << back - normal * headHalf;
        p.setPen(QPen(shape.color, shape.width * 0.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); // softer corners
        p.setBrush(shape.color);
        p.drawPolygon(triangle);
        break;
    }
    case ShapeType::Rect:
        p.setPen(QPen(shape.color, shape.width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRect(shape.rect.normalized());
        break;
    case ShapeType::Text: {
        if (shape.text.isEmpty() || shape.fontPx <= 0)
            break;
        // Not QStringLiteral: Qt's font caches keep the family name after the plugin is unloaded.
        QFont font(QString::fromLatin1("Segoe UI"));
        font.setWeight(QFont::DemiBold);
        font.setPixelSize(qMax(1, qRound(shape.fontPx)));
        const QFontMetricsF fm(font);
        QPainterPath        path;
        path.addText(QPointF(0, fm.ascent()), font, shape.text);
        p.translate(shape.rect.topLeft());
        p.rotate(-shape.textRotation);
        p.setPen(QPen(outlineFor(shape.color), shape.fontPx / 8.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(shape.color);
        p.drawPath(path);
        break;
    }
    case ShapeType::Pixelate:
    case ShapeType::BlackBox:
        break; // pixel operations (render())
    }
    p.restore();
}

int pixelBlock(const QRectF& rect)
{
    const QRectF r = rect.normalized();
    return qMax(16, qRound(qMin(r.width(), r.height()) / 8.0));
}

void pixelate(QImage& target, const QTransform& baseToTarget, const QRectF& rect, quint32 seed)
{
    const QRectF r = rect.normalized();
    if (r.isEmpty() || target.isNull())
        return;
    ensureRenderFormat(target);
    const bool  opaque = target.format() == QImage::Format_RGB32;
    const int   block  = pixelBlock(r);
    const int   cols   = static_cast<int>(std::ceil(r.width() / block));
    const int   rows   = static_cast<int>(std::ceil(r.height() / block));
    const QRect bounds = target.rect();
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const QRectF cell   = QRectF(r.left() + col * block, r.top() + row * block, block, block) & r;
            const QRect  pixels = pixelsInside(baseToTarget.mapRect(cell)) & bounds;
            if (pixels.isEmpty())
                continue;
            quint64 sum[4] = {0, 0, 0, 0};
            for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
                const QRgb* line = reinterpret_cast<const QRgb*>(target.constScanLine(y));
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const QRgb c = line[x];
                    sum[0] += static_cast<quint64>(qAlpha(c));
                    sum[1] += static_cast<quint64>(qRed(c));
                    sum[2] += static_cast<quint64>(qGreen(c));
                    sum[3] += static_cast<quint64>(qBlue(c));
                }
            }
            const quint64 n = static_cast<quint64>(pixels.width()) * static_cast<quint64>(pixels.height());
            const int     a = opaque ? 255 : (sum[0] / n >= 255 ? 255 : quantize(static_cast<int>(sum[0] / n), blockNoise(seed, col, row, 0), 255));
            const QRgb    fill = qRgba(quantize(static_cast<int>(sum[1] / n), blockNoise(seed, col, row, 1), a),
                                       quantize(static_cast<int>(sum[2] / n), blockNoise(seed, col, row, 2), a),
                                       quantize(static_cast<int>(sum[3] / n), blockNoise(seed, col, row, 3), a), a);
            for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
                QRgb* line = reinterpret_cast<QRgb*>(target.scanLine(y));
                std::fill(line + pixels.left(), line + pixels.right() + 1, fill);
            }
        }
    }
}

void blackBox(QImage& target, const QTransform& baseToTarget, const QRectF& rect)
{
    if (target.isNull())
        return;
    ensureRenderFormat(target);
    const QRect pixels = pixelsInside(baseToTarget.mapRect(rect.normalized())) & target.rect();
    if (pixels.isEmpty())
        return;
    const QRgb black = qRgba(0, 0, 0, 255);
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(target.scanLine(y));
        std::fill(line + pixels.left(), line + pixels.right() + 1, black);
    }
}

// ---- crop helpers --------------------------------------------------------------------------------------

QRect fitAspect(const QRect& bounds, double aspect, const QPointF& center)
{
    if (bounds.isEmpty() || aspect <= 0)
        return bounds;
    double w = bounds.width();
    double h = w / aspect;
    if (h > bounds.height()) {
        h = bounds.height();
        w = h * aspect;
    }
    const int iw   = qBound(1, qRound(w), bounds.width());
    const int ih   = qBound(1, qRound(h), bounds.height());
    const int left = qBound(bounds.left(), qRound(center.x() - iw / 2.0), bounds.left() + bounds.width() - iw);
    const int top  = qBound(bounds.top(), qRound(center.y() - ih / 2.0), bounds.top() + bounds.height() - ih);
    return QRect(left, top, iw, ih);
}

QRect dragCrop(const QRect& start, Handle handle, const QPointF& delta, const QRect& bounds, double aspect, int minSide)
{
    if (bounds.isEmpty() || handle == Handle::None)
        return start;
    const double bL   = bounds.left();
    const double bT   = bounds.top();
    const double bR   = bounds.left() + bounds.width();
    const double bB   = bounds.top() + bounds.height();
    const double minW = qMin<double>(minSide, bounds.width());
    const double minH = qMin<double>(minSide, bounds.height());
    double       L    = start.left();
    double       T    = start.top();
    double       R    = start.left() + start.width();
    double       B    = start.top() + start.height();
    const double dx   = delta.x();
    const double dy   = delta.y();

    const auto result = [&bounds](double l, double t, double r, double b) {
        const int x = qRound(l);
        const int y = qRound(t);
        return QRect(x, y, qRound(r) - x, qRound(b) - y) & bounds;
    };

    if (handle == Handle::Move) {
        const double mx = qBound(bL - L, dx, bR - R);
        const double my = qBound(bT - T, dy, bB - B);
        return result(L + mx, T + my, R + mx, B + my);
    }

    const bool left   = handle == Handle::Left || handle == Handle::TopLeft || handle == Handle::BottomLeft;
    const bool right  = handle == Handle::Right || handle == Handle::TopRight || handle == Handle::BottomRight;
    const bool top    = handle == Handle::Top || handle == Handle::TopLeft || handle == Handle::TopRight;
    const bool bottom = handle == Handle::Bottom || handle == Handle::BottomLeft || handle == Handle::BottomRight;

    if (aspect <= 0) {
        if (left)
            L = qBound(bL, L + dx, R - minW);
        if (right)
            R = qBound(L + minW, R + dx, bR);
        if (top)
            T = qBound(bT, T + dy, B - minH);
        if (bottom)
            B = qBound(T + minH, B + dy, bB);
        return result(L, T, R, B);
    }

    // With an aspect: the smallest size that keeps it and both minimums.
    const double minWa = qMax(minW, minH * aspect);
    if ((left || right) && (top || bottom)) {
        // A corner: the opposite corner stays; the larger change decides the size.
        const double ax   = left ? R : L;
        const double ay   = top ? B : T;
        const double sx   = left ? -1.0 : 1.0;
        const double sy   = top ? -1.0 : 1.0;
        const double mx   = (left ? L : R) + dx;
        const double my   = (top ? T : B) + dy;
        double       w    = qMax(0.0, (mx - ax) * sx);
        double       h    = qMax(0.0, (my - ay) * sy);
        if (w / aspect > h)
            h = w / aspect;
        else
            w = h * aspect;
        const double maxW = qMin(sx > 0 ? bR - ax : ax - bL, (sy > 0 ? bB - ay : ay - bT) * aspect);
        w                 = qMin(qMax(w, minWa), maxW);
        h                 = w / aspect;
        L                 = sx > 0 ? ax : ax - w;
        R                 = L + w;
        T                 = sy > 0 ? ay : ay - h;
        B                 = T + h;
        return result(L, T, R, B);
    }
    if (left || right) {
        // A side: the opposite side stays, the height follows, centred where it was (moved in if needed).
        const double ax   = left ? R : L;
        const double maxW = qMin(left ? ax - bL : bR - ax, (bB - bT) * aspect);
        double       w    = left ? R - (L + dx) : (R + dx) - L;
        w                 = qMin(qMax(w, minWa), maxW);
        const double h    = w / aspect;
        const double cy   = (T + B) / 2.0;
        T                 = qBound(bT, cy - h / 2.0, bB - h);
        B                 = T + h;
        L                 = left ? ax - w : ax;
        R                 = L + w;
        return result(L, T, R, B);
    }
    const double ay   = top ? B : T;
    const double maxH = qMin(top ? ay - bT : bB - ay, (bR - bL) / aspect);
    double       h    = top ? B - (T + dy) : (B + dy) - T;
    h                 = qMin(qMax(h, minWa / aspect), maxH);
    const double w    = h * aspect;
    const double cx   = (L + R) / 2.0;
    L                 = qBound(bL, cx - w / 2.0, bR - w);
    R                 = L + w;
    T                 = top ? ay - h : ay;
    B                 = T + h;
    return result(L, T, R, B);
}

// ---- opening and saving ------------------------------------------------------------------------------

QString openProblemText(OpenProblem problem)
{
    switch (problem) {
    case OpenProblem::Animated:
        return i18n::t("Animated images can't be edited");
    case OpenProblem::TooLarge:
        return i18n::t("Too large to edit here (over %1 megapixels)").arg(kMaxPixels / 1000000);
    case OpenProblem::Unreadable:
        return i18n::t("This image couldn't be opened");
    case OpenProblem::None:
        break;
    }
    return {};
}

QImage loadForEditing(const QString& path, OpenProblem* problem)
{
    const auto fail = [problem](OpenProblem why) {
        if (problem)
            *problem = why;
        return QImage();
    };
    if (problem)
        *problem = OpenProblem::None;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true); // EXIF orientation: the picture is edited (and exported) upright
    if (!reader.canRead())
        return fail(OpenProblem::Unreadable);
    if (reader.supportsAnimation() && reader.imageCount() > 1)
        return fail(OpenProblem::Animated);
    const QSize size = reader.size(); // the header only
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaxPixels)
        return fail(OpenProblem::TooLarge);
    QImage image = reader.read();
    if (image.isNull())
        return fail(OpenProblem::Unreadable);
    if (static_cast<qint64>(image.width()) * image.height() > kMaxPixels)
        return fail(OpenProblem::TooLarge);
    return image.convertToFormat(formatFor(image));
}

bool hasTransparency(const QImage& image)
{
    if (image.isNull() || !image.hasAlphaChannel())
        return false;
    const QImage argb = image.format() == QImage::Format_ARGB32 || image.format() == QImage::Format_ARGB32_Premultiplied
                            ? image
                            : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) != 255)
                return true;
        }
    }
    return false;
}

Encoded encode(const QImage& image, const QString& sourceName, bool pasted, bool convertLargePngToJpeg)
{
    Encoded out;
    if (image.isNull())
        return out;
    const QString suffix      = QFileInfo(sourceName).suffix().toLower();
    const bool    transparent = hasTransparency(image);
    // Fully opaque pixels need no alpha channel in the file.
    const QImage opaque = transparent ? image : image.convertToFormat(QImage::Format_RGB32);
    QBuffer      buffer(&out.data);
    buffer.open(QIODevice::WriteOnly);
    const bool photo = !pasted && !transparent && (isJpegSuffix(suffix) || suffix == QLatin1String("webp"));
    if (photo) {
        if (!opaque.save(&buffer, "JPG", kPhotoQuality))
            return {};
        out.extension = isJpegSuffix(suffix) ? suffix : QStringLiteral("jpg");
        return out;
    }
    if (!opaque.save(&buffer, "PNG"))
        return {};
    out.extension = QStringLiteral("png");
    if (convertLargePngToJpeg && !transparent && out.data.size() > kLargePngBytes) {
        out.data.clear();
        buffer.seek(0);
        if (!opaque.save(&buffer, "JPG", kPngToJpegQuality))
            return {};
        out.extension = QStringLiteral("jpg");
    }
    return out;
}

QString exportName(const QString& sourceName, const QString& extension)
{
    const QFileInfo fi(sourceName);
    QString         base = fi.completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("image");
    const QString own  = fi.suffix();
    const bool    same = own.compare(extension, Qt::CaseInsensitive) == 0
                      || (isJpegSuffix(own.toLower()) && isJpegSuffix(extension.toLower()));
    return base + QLatin1Char('.') + (same ? own : extension);
}

QString writeEdited(const Encoded& encoded, const QString& dir, const QString& sourceName)
{
    if (encoded.data.isEmpty() || encoded.extension.isEmpty() || !QDir().mkpath(dir))
        return {};
    const QString path = QDir(dir).filePath(exportName(sourceName, encoded.extension));
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(encoded.data) != encoded.data.size() || !file.flush()) {
        file.close();
        QFile::remove(path);
        return {};
    }
    file.close();
    return path;
}

} // namespace imageedit
