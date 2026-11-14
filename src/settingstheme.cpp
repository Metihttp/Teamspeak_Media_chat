#include "settingstheme.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVariant>
#include <QWidget>

#include "uiutil.h"

namespace settingstheme {

bool isDark(const QPalette& palette)
{
    // TeamSpeak's dark skins colour everything by style sheet; once polished, the palette shows those colours.
    return palette.color(QPalette::WindowText).lightness() > 170 || palette.color(QPalette::Window).lightness() < 128;
}

QColor mutedText(const QPalette& palette, double start, double minContrast)
{
    const QColor text       = palette.color(QPalette::Active, QPalette::WindowText);
    const QColor background = palette.color(QPalette::Active, QPalette::Window);
    for (int step = qRound(start * 20); step > 0; --step) {
        const double t = step / 20.0;
        const QColor color(qRound(text.red() + (background.red() - text.red()) * t), qRound(text.green() + (background.green() - text.green()) * t),
                           qRound(text.blue() + (background.blue() - text.blue()) * t));
        if (ui::contrastRatio(color, background) >= minContrast)
            return color;
    }
    return text;
}

Colors roleColors(const QPalette& palette)
{
    Colors       c;
    const QColor background = palette.color(QPalette::Active, QPalette::Window);
    const QColor text       = palette.color(QPalette::Active, QPalette::WindowText);
    c.dark                  = isDark(palette);
    c.hint                  = mutedText(palette, 0.4, 4.6);
    c.error                 = c.dark ? QColor(0xFF, 0x99, 0xA4) : QColor(0xC4, 0x2B, 0x1C);
    if (ui::contrastRatio(c.error, background) < 4.5)
        c.error = text; // the icon still marks it
    // Live test fix: the light green is #157032, not #1a7f37: that one reads at 4.46:1 on TeamSpeak's light skin
    // (#f0f0f0), so the rule below always swapped it for the text colour there (a black check mark, black text).
    c.ok = c.dark ? QColor(0x57, 0xD3, 0x8C) : QColor(0x15, 0x70, 0x32);
    if (ui::contrastRatio(c.ok, background) < 4.5)
        c.ok = text; // the check mark still marks it
    return c;
}

void applyLabelRoles(QWidget* root, const Colors& colors)
{
    if (!root)
        return;
    for (QLabel* label : root->findChildren<QLabel*>()) {
        const QString role = label->property("role").toString();
        if (role.isEmpty())
            continue;
        const QColor color   = role == QLatin1String("error") ? colors.error : role == QLatin1String("ok") ? colors.ok : colors.hint;
        QPalette     palette = label->palette();
        palette.setColor(QPalette::Active, QPalette::WindowText, color);
        palette.setColor(QPalette::Inactive, QPalette::WindowText, color);
        label->setPalette(palette);
    }
}

QString roleSheet(const QString& objectName, const Colors& colors)
{
    const QString n = QLatin1Char('#') + objectName;
    return QString::fromLatin1("%1 QLabel[role=\"hint\"]{color:%2;}%1 QLabel[role=\"error\"]{color:%3;}%1 QLabel[role=\"ok\"]{color:%4;}")
        .arg(n, colors.hint.name(), colors.error.name(), colors.ok.name());
}

QPixmap checkMark(const QColor& color, int size, qreal dpr)
{
    const qreal r = dpr > 0 ? dpr : 1.0;
    QPixmap     pixmap(qMax(1, qRound(size * r)), qMax(1, qRound(size * r)));
    pixmap.setDevicePixelRatio(r);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath path;
    path.moveTo(size * 0.20, size * 0.53);
    path.lineTo(size * 0.42, size * 0.74);
    path.lineTo(size * 0.82, size * 0.28);
    p.drawPath(path);
    return pixmap;
}

} // namespace settingstheme
