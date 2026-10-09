#pragma once

// BlurHash (https://blurha.sh): a ~20-30 character string describing a blurred placeholder of an
// image. Sent inside the chat link so receivers can show a correctly-shaped, coloured placeholder
// before the real media has downloaded.

#include <QImage>
#include <QSize>
#include <QString>

namespace blurhash {

// Encodes an image (downscale it to <= 64px first for speed). componentsX/Y must be in 1..9.
// Returns an empty string for a null image or invalid component counts.
QString encode(const QImage& image, int componentsX = 4, int componentsY = 3);

// Decodes to an image of the given pixel size (Format_RGB32). Returns a null image if the hash is
// invalid or the size is empty. punch > 1 increases contrast.
QImage decode(const QString& hash, const QSize& size, double punch = 1.0);

bool isValid(const QString& hash);

} // namespace blurhash
