#include "filenames.h"

#include <QFileInfo>

#include "medialink.h"

namespace filenames {

namespace {

constexpr int kMaxExtensionLength = 16;
constexpr int kMinNameLength      = 8; // never shorter than this, however deep the folder is

} // namespace

QString chopTrailingDotsAndSpaces(QString name)
{
    while (!name.isEmpty() && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))))
        name.chop(1);
    return name;
}

bool isReservedDeviceName(const QString& name)
{
    // The part before the first dot counts ("nul.tar.gz" is NUL), without trailing spaces ("CON .txt").
    QString stem = name.section(QLatin1Char('.'), 0, 0);
    while (stem.endsWith(QLatin1Char(' ')))
        stem.chop(1);
    stem = stem.toUpper();
    if (stem == QLatin1String("CON") || stem == QLatin1String("PRN") || stem == QLatin1String("AUX") || stem == QLatin1String("NUL")
        || stem == QLatin1String("CONIN$") || stem == QLatin1String("CONOUT$"))
        return true;
    if (stem.size() != 4 || !(stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT"))))
        return false;
    const ushort last = stem.at(3).unicode();
    return (last >= '0' && last <= '9') || last == 0x00B9 || last == 0x00B2 || last == 0x00B3; // ¹ ² ³
}

QString safeLocalFileName(const QString& name, int maxLength)
{
    static const QLatin1String reserved("<>:\"/\\|?*");
    QString                    result;
    result.reserve(name.size() + 1);
    for (const QChar c : name)
        result += (c.unicode() < 0x20 || c.unicode() == 0x7f || QString(reserved).contains(c)) ? QChar(QLatin1Char('_')) : c;
    result = chopTrailingDotsAndSpaces(result);
    if (isReservedDeviceName(result))
        result.prepend(QLatin1Char('_'));

    maxLength = qMax(kMinNameLength, maxLength);
    if (result.length() > maxLength) {
        const QString ext  = QFileInfo(result).suffix().left(kMaxExtensionLength);
        const bool    keep = !ext.isEmpty() && ext.length() + 2 <= maxLength;
        QString       base = result.left(keep ? maxLength - ext.length() - 1 : maxLength);
        // A cut never ends in half a surrogate pair, or in dots / spaces ("name...zip").
        if (!base.isEmpty() && base.at(base.length() - 1).isHighSurrogate())
            base.chop(1);
        base   = chopTrailingDotsAndSpaces(base);
        result = keep ? (base.isEmpty() ? QString::fromLatin1("file") : base) + QLatin1Char('.') + ext : base;
    }
    return result.isEmpty() ? QString::fromLatin1("file") : result;
}

QString exportFileName(const MediaLink& link, int folderLength, int maxPathLength)
{
    return safeLocalFileName(displayNameFor(link), qMin(120, maxPathLength - qMax(0, folderLength)));
}

} // namespace filenames
