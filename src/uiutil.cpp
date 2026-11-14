#include "uiutil.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QLocale>

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

QColor readableOn(const QColor& color, const QVector<QColor>& backs, double minimum)
{
    QColor c = color;
    c.setAlpha(255);
    if (backs.isEmpty())
        return c;
    const auto worst = [&backs](const QColor& fore) {
        double low = 99;
        for (const QColor& b : backs)
            low = qMin(low, contrastRatio(fore, b));
        return low;
    };
    const QColor target = backs.first().lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    for (int step = 0; step < 60 && worst(c) < minimum; ++step)
        c = QColor::fromRgbF(c.redF() + (target.redF() - c.redF()) * 0.03, c.greenF() + (target.greenF() - c.greenF()) * 0.03,
                             c.blueF() + (target.blueF() - c.blueF()) * 0.03);
    return c;
}

QString savedToText(const QString& savedPath)
{
    const QFileInfo info(savedPath);
    QString         folder = info.absoluteDir().dirName();
    if (folder.isEmpty())
        folder = QDir::toNativeSeparators(info.absolutePath()); // a drive root: "C:\"
    return i18n::t("Saved to %1").arg(folder);
}

QString whenText(const QDateTime& utc)
{
    const QDateTime local = utc.toLocalTime();
    const QLocale   english(QLocale::English);
    const QString   time  = english.toString(local.time(), QString::fromLatin1("HH:mm"));
    const QDate     today = QDate::currentDate();
    if (local.date() == today)
        return i18n::t("today at %1").arg(time);
    if (local.date() == today.addDays(-1))
        return i18n::t("yesterday at %1").arg(time);
    return i18n::t("%1 at %2").arg(english.toString(local.date(), QString::fromLatin1("MMM d")), time);
}

} // namespace ui
