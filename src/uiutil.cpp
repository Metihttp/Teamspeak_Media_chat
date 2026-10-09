#include "uiutil.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

#include "i18n.h"

#include <windows.h>

namespace ui {

namespace {

// sRGB channel (0..1) to linear light, as WCAG 2 defines it.
double linear(double channel)
{
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& color)
{
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

} // namespace

bool animationsEnabled()
{
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return true;
    return enabled != FALSE;
}

int notificationDurationMs()
{
    ULONG seconds = 0;
    if (!SystemParametersInfoW(SPI_GETMESSAGEDURATION, 0, &seconds, 0) || seconds == 0)
        return 5000;
    return static_cast<int>(qMin<ULONG>(seconds, 3600) * 1000);
}

double contrastRatio(const QColor& a, const QColor& b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QColor flatten(const QColor& color, const QColor& background)
{
    const double alpha = color.alphaF();
    auto         mix   = [alpha](double over, double under) { return over * alpha + under * (1.0 - alpha); };
    return QColor::fromRgbF(mix(color.redF(), background.redF()), mix(color.greenF(), background.greenF()), mix(color.blueF(), background.blueF()));
}

QString savedToText(const QString& savedPath)
{
    const QFileInfo info(savedPath);
    QString         folder = info.absoluteDir().dirName();
    if (folder.isEmpty())
        folder = QDir::toNativeSeparators(info.absolutePath()); // a drive root: "C:\"
    return i18n::t("Saved to %1").arg(folder);
}

} // namespace ui
