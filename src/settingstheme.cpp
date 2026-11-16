#include "settingstheme.h"

#include <QComboBox>
#include <QFontInfo>
#include <QGradient>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVariant>
#include <QWidget>

#include "darkmode.h"    // 2.7 dark mode: DarkMode::isActive()
#include "darkpalette.h" // 2.7 dark mode: its Settings tokens
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

// ---- 2.7 F3: tokens ---------------------------------------------------------------------------------------------------
namespace settingstheme {

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor(qRound(a.red() + (b.red() - a.red()) * t), qRound(a.green() + (b.green() - a.green()) * t), qRound(a.blue() + (b.blue() - a.blue()) * t));
}

QColor solidOf(const QBrush& brush, const QColor& fallback)
{
    if (const QGradient* g = brush.gradient()) {
        const QGradientStops stops = g->stops();
        if (stops.isEmpty())
            return fallback;
        double r = 0, gr = 0, b = 0;
        for (const QGradientStop& stop : stops) {
            r += stop.second.redF();
            gr += stop.second.greenF();
            b += stop.second.blueF();
        }
        const double n = stops.size();
        return QColor::fromRgbF(r / n, gr / n, b / n);
    }
    if (brush.style() == Qt::SolidPattern && brush.color().isValid())
        return QColor(brush.color().rgb()); // opaque
    return fallback;
}

qreal sizeFactor(const QFont& font, const QWidget* widget)
{
    qreal pixels = font.pixelSize();
    if (pixels <= 0) {
        const qreal dpi = widget ? widget->logicalDpiY() : 96.0;
        pixels          = font.pointSizeF() > 0 ? font.pointSizeF() * dpi / 72.0 : QFontInfo(font).pixelSize();
    }
    return qMax<qreal>(1.0, pixels / 11.0);
}

namespace {

// The text colour moved toward bg as far as start allows while it keeps minimum:1 on bg.
QColor toward(const QColor& text, const QColor& bg, double start, double minimum)
{
    for (int step = qRound(start * 40); step > 0; --step) {
        const QColor c = mix(text, bg, step / 40.0);
        if (ui::contrastRatio(c, bg) >= minimum)
            return c;
    }
    return text;
}

} // namespace

Tokens tokens(const QPalette& palette, const QColor& skinButton, qreal k)
{
    Tokens t;
    t.k      = k > 0 ? k : 1.0;
    t.text   = solidOf(palette.brush(QPalette::Active, QPalette::WindowText), QColor(Qt::black));
    t.window = solidOf(palette.brush(QPalette::Active, QPalette::Window), QColor(0xf0, 0xf0, 0xf0));
    t.dark   = t.text.lightness() > 170 || t.window.lightness() < 128;
    if (t.dark) {
        t.card   = mix(t.window, t.text, 0.045);
        t.border = mix(t.window, t.text, 0.13);
    } else if (t.window.lightness() >= 248) {
        t.card   = mix(t.window, t.text, 0.03);
        t.border = mix(t.window, t.text, 0.15);
    } else {
        t.card   = mix(t.window, QColor(Qt::white), 0.8);
        t.border = mix(t.window, t.text, 0.12);
    }
    t.sidebar = t.dark ? mix(t.window, QColor(Qt::black), 0.12) : mix(t.window, t.text, 0.025);
    t.divider = mix(t.card, t.text, t.dark ? 0.10 : 0.08);
    // Secondary text sits on cards, on the window (page descriptions, the unsaved note) and on the sidebar (the version).
    t.muted = t.text;
    for (int step = 18; step > 0; --step) {
        const QColor c = mix(t.text, t.card, step / 40.0);
        if (ui::contrastRatio(c, t.card) >= 4.6 && ui::contrastRatio(c, t.window) >= 4.6 && ui::contrastRatio(c, t.sidebar) >= 4.6) {
            t.muted = c;
            break;
        }
    }
    t.disabled            = toward(t.text, t.card, 0.7, 3.0);
    t.control             = toward(t.text, t.card, 0.5, 3.0);
    const QColor cands[]  = {skinButton, solidOf(palette.brush(QPalette::Active, QPalette::Highlight), QColor()),
                            solidOf(palette.brush(QPalette::Active, QPalette::Link), QColor())};
    for (const QColor& c : cands) {
        if (c.isValid() && c.hsvSaturation() > 60 && ui::contrastRatio(c, t.card) >= 3.0) {
            t.accent = c;
            break;
        }
    }
    if (!t.accent.isValid()) {
        QColor c = solidOf(palette.brush(QPalette::Active, QPalette::Highlight), QColor(0x00, 0x78, 0xd4));
        for (int i = 0; i < 40 && ui::contrastRatio(c, t.card) < 3.0; ++i)
            c = mix(c, t.text, 0.08);
        t.accent = c;
    }
    t.onAccent = ui::contrastRatio(QColor(Qt::white), t.accent) >= 3.0 ? QColor(Qt::white) : QColor(Qt::black);
    t.focus    = t.text;
    t.navSel   = mix(t.sidebar, t.accent, t.dark ? 0.24 : 0.14);
    t.navHover = mix(t.sidebar, t.text, 0.07);
    return t;
}

Tokens tokensFor(const QWidget* w)
{
    const QWidget* top = w ? w->window() : nullptr;
    if (const auto* source = dynamic_cast<const TokenSource*>(top))
        return source->themeTokens();
    if (DarkMode::isActive()) // 2.7 dark mode: the theme's own tokens (the Chat proxy window, sections on their own)
        return darkpalette::settingsTokens(top ? sizeFactor(top->font(), top) : 1.0);
    if (!top)
        return tokens(QPalette(), QColor(), 1.0);
    return tokens(top->palette(), QColor(), sizeFactor(top->font(), top));
}

bool showsFocus(const QWidget* w)
{
    if (!w || !w->hasFocus())
        return false;
    if (const auto* source = dynamic_cast<const TokenSource*>(w->window()))
        return source->keyboardFocusShown();
    return true;
}

int comboFitWidth(const QComboBox* combo)
{
    if (!combo)
        return 0;
    const QSize hint = combo->sizeHint();
    if (combo->isEditable() || combo->count() == 0)
        return hint.width();
    const QFontMetrics fm   = combo->fontMetrics();
    int                text = 0;
    bool               icon = false;
    for (int i = 0; i < combo->count(); ++i) {
        text = qMax(text, fm.horizontalAdvance(combo->itemText(i)));
        icon = icon || !combo->itemIcon(i).isNull();
    }
    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    opt.editable = false;
    opt.frame    = combo->hasFrame();
    opt.iconSize = combo->iconSize();
    opt.rect     = QRect(QPoint(), hint);
    const QRect field = combo->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo);
    // CE_ComboBoxLabel: the icon and 4 px before the text, which keeps 1 px on either side.
    const int need = text + 2 + (icon ? combo->iconSize().width() + 4 : 0) + fm.horizontalAdvance(QLatin1Char(' '));
    return field.isValid() && need > field.width() ? hint.width() + need - field.width() : hint.width();
}

} // namespace settingstheme
