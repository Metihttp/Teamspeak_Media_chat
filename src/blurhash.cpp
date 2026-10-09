#include "blurhash.h"

#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>

// Port of the reference implementation (https://github.com/woltapp/blurhash, MIT licence).

namespace blurhash {

namespace {

constexpr double kPi = 3.14159265358979323846;

const char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

int decode83Char(QChar c)
{
    const ushort u = c.unicode();
    if (u > 127)
        return -1;
    static const std::array<signed char, 128> table = [] {
        std::array<signed char, 128> t{};
        t.fill(-1);
        for (int i = 0; i < 83; ++i)
            t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<signed char>(i);
        return t;
    }();
    return table[u];
}

// Callers validate the characters first (isValid).
int decode83(const QString& text, int from, int length)
{
    int value = 0;
    for (int i = from; i < from + length; ++i)
        value = value * 83 + decode83Char(text.at(i));
    return value;
}

void encode83(int value, int length, QString& out)
{
    for (int i = 1; i <= length; ++i) {
        int divisor = 1;
        for (int k = 0; k < length - i; ++k)
            divisor *= 83;
        out += QLatin1Char(kAlphabet[(value / divisor) % 83]);
    }
}

double srgbToLinear(int value)
{
    const double v = value / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

const std::array<double, 256>& srgbToLinearTable()
{
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[static_cast<size_t>(i)] = srgbToLinear(i);
        return t;
    }();
    return table;
}

int linearToSrgb(double value)
{
    const double v = std::clamp(value, 0.0, 1.0);
    if (v <= 0.0031308)
        return static_cast<int>(v * 12.92 * 255.0 + 0.5);
    return static_cast<int>((1.055 * std::pow(v, 1.0 / 2.4) - 0.055) * 255.0 + 0.5);
}

double signPow(double value, double exp)
{
    return std::copysign(std::pow(std::abs(value), exp), value);
}

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

int encodeDC(const Rgb& c)
{
    return (linearToSrgb(c.r) << 16) + (linearToSrgb(c.g) << 8) + linearToSrgb(c.b);
}

int encodeAC(const Rgb& c, double maximumValue)
{
    auto quant = [maximumValue](double v) {
        return static_cast<int>(std::clamp(std::floor(signPow(v / maximumValue, 0.5) * 9.0 + 9.5), 0.0, 18.0));
    };
    return quant(c.r) * 19 * 19 + quant(c.g) * 19 + quant(c.b);
}

Rgb decodeDC(int value)
{
    // Four base83 digits can exceed 24 bits in a hostile hash; keep each channel in 0..255.
    return {srgbToLinear(std::min(value >> 16, 255)), srgbToLinear((value >> 8) & 255), srgbToLinear(value & 255)};
}

Rgb decodeAC(int value, double maximumValue)
{
    const int quantR = value / (19 * 19);
    const int quantG = (value / 19) % 19;
    const int quantB = value % 19;
    return {signPow((quantR - 9) / 9.0, 2.0) * maximumValue, signPow((quantG - 9) / 9.0, 2.0) * maximumValue,
            signPow((quantB - 9) / 9.0, 2.0) * maximumValue};
}

// cos(pi * i * x / size) for every component i and pixel x, laid out as [i * size + x].
QVector<double> cosineTable(int components, int size)
{
    QVector<double> table(components * size);
    for (int i = 0; i < components; ++i) {
        for (int x = 0; x < size; ++x)
            table[i * size + x] = std::cos(kPi * i * x / size);
    }
    return table;
}

} // namespace

QString encode(const QImage& image, int componentsX, int componentsY)
{
    if (image.isNull() || componentsX < 1 || componentsX > 9 || componentsY < 1 || componentsY > 9)
        return {};

    // Premultiplied pixels are the image composited onto black, which matches the preview JPEGs.
    const QImage src    = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int    width  = src.width();
    const int    height = src.height();
    if (width <= 0 || height <= 0)
        return {};

    const auto&           lin  = srgbToLinearTable();
    const QVector<double> cosX = cosineTable(componentsX, width);
    const QVector<double> cosY = cosineTable(componentsY, height);

    const int    count = componentsX * componentsY;
    QVector<Rgb> factors(count);
    QVector<Rgb> row(componentsX);
    for (int y = 0; y < height; ++y) {
        // Sum the row against the horizontal basis first, then distribute over the vertical one.
        std::fill(row.begin(), row.end(), Rgb{});
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const double r = lin[static_cast<size_t>(qRed(line[x]))];
            const double g = lin[static_cast<size_t>(qGreen(line[x]))];
            const double b = lin[static_cast<size_t>(qBlue(line[x]))];
            for (int i = 0; i < componentsX; ++i) {
                const double basis = cosX[i * width + x];
                row[i].r += basis * r;
                row[i].g += basis * g;
                row[i].b += basis * b;
            }
        }
        for (int j = 0; j < componentsY; ++j) {
            const double basis = cosY[j * height + y];
            for (int i = 0; i < componentsX; ++i) {
                Rgb& f = factors[j * componentsX + i];
                f.r += basis * row[i].r;
                f.g += basis * row[i].g;
                f.b += basis * row[i].b;
            }
        }
    }

    const double pixels = static_cast<double>(width) * height;
    for (int c = 0; c < count; ++c) {
        const double scale = (c == 0 ? 1.0 : 2.0) / pixels;
        factors[c].r *= scale;
        factors[c].g *= scale;
        factors[c].b *= scale;
    }

    QString hash;
    hash.reserve(4 + 2 * count);
    encode83((componentsX - 1) + (componentsY - 1) * 9, 1, hash);

    double maximumValue = 1.0;
    if (count > 1) {
        double actualMax = 0.0;
        for (int c = 1; c < count; ++c)
            actualMax = std::max({actualMax, std::abs(factors[c].r), std::abs(factors[c].g), std::abs(factors[c].b)});
        const int quantisedMax = static_cast<int>(std::clamp(std::floor(actualMax * 166.0 - 0.5), 0.0, 82.0));
        maximumValue           = (quantisedMax + 1) / 166.0;
        encode83(quantisedMax, 1, hash);
    } else {
        encode83(0, 1, hash);
    }

    encode83(encodeDC(factors[0]), 4, hash);
    for (int c = 1; c < count; ++c)
        encode83(encodeAC(factors[c], maximumValue), 2, hash);
    return hash;
}

QImage decode(const QString& hash, const QSize& size, double punch)
{
    if (!isValid(hash) || size.isEmpty())
        return {};

    if (!(punch > 0.0) || !std::isfinite(punch))
        punch = 1.0;
    const int    sizeFlag = decode83(hash, 0, 1);
    const int    numY     = sizeFlag / 9 + 1;
    const int    numX     = sizeFlag % 9 + 1;
    const int    count    = numX * numY;
    const double maximum  = (decode83(hash, 1, 1) + 1) / 166.0 * punch;

    QVector<Rgb> colors(count);
    colors[0] = decodeDC(decode83(hash, 2, 4));
    for (int c = 1; c < count; ++c)
        colors[c] = decodeAC(decode83(hash, 4 + c * 2, 2), maximum);

    const int width  = size.width();
    const int height = size.height();
    QImage    image(width, height, QImage::Format_RGB32);
    if (image.isNull())
        return {};

    const QVector<double> cosX = cosineTable(numX, width);
    const QVector<double> cosY = cosineTable(numY, height);

    QVector<Rgb> rowColors(numX);
    for (int y = 0; y < height; ++y) {
        // Collapse the vertical basis for this row so each pixel only needs numX terms.
        for (int i = 0; i < numX; ++i) {
            Rgb sum;
            for (int j = 0; j < numY; ++j) {
                const double basis = cosY[j * height + y];
                const Rgb&   c     = colors[j * numX + i];
                sum.r += c.r * basis;
                sum.g += c.g * basis;
                sum.b += c.b * basis;
            }
            rowColors[i] = sum;
        }
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            Rgb px;
            for (int i = 0; i < numX; ++i) {
                const double basis = cosX[i * width + x];
                px.r += rowColors[i].r * basis;
                px.g += rowColors[i].g * basis;
                px.b += rowColors[i].b * basis;
            }
            line[x] = qRgb(linearToSrgb(px.r), linearToSrgb(px.g), linearToSrgb(px.b));
        }
    }
    return image;
}

bool isValid(const QString& hash)
{
    if (hash.length() < 6)
        return false;
    for (const QChar c : hash) {
        if (decode83Char(c) < 0)
            return false;
    }
    const int sizeFlag = decode83(hash, 0, 1);
    if (sizeFlag > 80) // more than 9 components vertically
        return false;
    const int numY = sizeFlag / 9 + 1;
    const int numX = sizeFlag % 9 + 1;
    return hash.length() == 4 + 2 * numX * numY;
}

} // namespace blurhash
