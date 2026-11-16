#include "chatcolors.h"

#include <QGradient>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOption>
#include <QVector>
#include <QWidget>

#include "settings.h"
#include "uiutil.h"

namespace chatcolors {

namespace {

constexpr int kSample = 24; // the sampled corner of the ChatTab's background, px

QColor opaque(const QColor& c, const QColor& under)
{
    if (!c.isValid())
        return c;
    if (c.alpha() == 255)
        return c;
    return ui::flatten(c, under.isValid() && under.alpha() == 255 ? under : QColor(Qt::white));
}

struct Tones {
    const char* light;
    const char* dark;
};

// The presets (design 4.2): each tone is then made 4.5:1 on the chat's background.
Tones tonesOf(const QString& choice)
{
    if (choice == QLatin1String("blue"))
        return {"#0b5cad", "#5eb0ff"};
    if (choice == QLatin1String("teal"))
        return {"#0f766e", "#3fc7b5"};
    if (choice == QLatin1String("green"))
        return {"#1e7b34", "#5fcf80"};
    if (choice == QLatin1String("orange"))
        return {"#a3510b", "#f2a65a"};
    if (choice == QLatin1String("pink"))
        return {"#a8326a", "#f48fc0"};
    return {"#4752c4", "#949cf7"}; // Automatic (2.6's colours)
}

} // namespace

Colors teamSpeakLight()
{
    Colors c;
    c.base = QColor(0xff, 0xff, 0xff);
    c.text = QColor(0x00, 0x00, 0x00);
    c.link = QColor(0x1c, 0x82, 0xcc);
    c.dark = false;
    return c;
}

bool isDark(const QColor& base)
{
    if (!base.isValid())
        return false;
    return ui::contrastRatio(QColor(Qt::white), base) >= ui::contrastRatio(QColor(Qt::black), base);
}

QColor averageOfStops(const QGradient& gradient)
{
    const QGradientStops stops = gradient.stops();
    if (stops.isEmpty())
        return {};
    qreal r = 0, g = 0, b = 0, a = 0;
    for (const QGradientStop& stop : stops) {
        r += stop.second.redF();
        g += stop.second.greenF();
        b += stop.second.blueF();
        a += stop.second.alphaF();
    }
    const qreal n = stops.size();
    return QColor::fromRgbF(r / n, g / n, b / n, a / n);
}

Colors fromPalette(const QPalette& pal)
{
    Colors        c;
    const QBrush& brush  = pal.brush(QPalette::Active, QPalette::Base);
    const QColor  window = pal.color(QPalette::Active, QPalette::Window);
    switch (brush.style()) {
    case Qt::SolidPattern:
        c.base = brush.color();
        break;
    case Qt::LinearGradientPattern:
    case Qt::RadialGradientPattern:
    case Qt::ConicalGradientPattern:
        if (brush.gradient())
            c.base = averageOfStops(*brush.gradient());
        break;
    default:
        break; // a texture or a pattern: the window colour until it is sampled
    }
    if (!c.base.isValid())
        c.base = window.isValid() ? window : QColor(Qt::white);
    c.base = opaque(c.base, window);
    if (!c.base.isValid() || c.base.alpha() != 255)
        c.base = QColor(Qt::white);
    c.text = pal.color(QPalette::Active, QPalette::Text);
    c.link = pal.color(QPalette::Active, QPalette::Link);
    c.dark = isDark(c.base);
    return c;
}

bool isTexture(const QPalette& pal)
{
    switch (pal.brush(QPalette::Active, QPalette::Base).style()) {
    case Qt::SolidPattern:
    case Qt::LinearGradientPattern:
    case Qt::RadialGradientPattern:
    case Qt::ConicalGradientPattern:
        return false;
    default:
        return true;
    }
}

Colors withSample(const Colors& fromPal, const QColor& sampled, bool texture)
{
    if (!sampled.isValid() || sampled.alpha() != 255)
        return fromPal;
    Colors c = fromPal;
    // Only when the palette can't be the painted colour: a texture (no colour of its own), or pixels that say the other of
    // dark and light (a picture only the style sheet paints). Otherwise the palette's exact colour stays (TeamSpeak's skins
    // keep their 2.6 colours to the bit; a gradient keeps the average of its stops).
    if (!texture && isDark(sampled) == fromPal.dark)
        return c;
    c.base    = sampled;
    c.dark    = isDark(sampled);
    c.sampled = true;
    return c;
}

QColor dominant(const QImage& image)
{
    if (image.isNull())
        return {};
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    struct Bucket {
        int    count = 0;
        qint64 r = 0, g = 0, b = 0;
    };
    QHash<int, Bucket> buckets;
    for (int y = 0; y < img.height(); y += 2) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 2) {
            const QRgb px = line[x];
            if (qAlpha(px) < 250)
                continue; // nothing (or a translucent edge) drawn there
            const int key = ((qRed(px) >> 4) << 8) | ((qGreen(px) >> 4) << 4) | (qBlue(px) >> 4);
            Bucket&   bk  = buckets[key];
            ++bk.count;
            bk.r += qRed(px);
            bk.g += qGreen(px);
            bk.b += qBlue(px);
        }
    }
    const Bucket* best = nullptr;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        if (!best || it->count > best->count)
            best = &it.value();
    }
    if (!best || best->count == 0)
        return {};
    return QColor(static_cast<int>(best->r / best->count), static_cast<int>(best->g / best->count), static_cast<int>(best->b / best->count));
}

QColor readable(const QColor& colour, const QColor& base, double minimum)
{
    if (!base.isValid())
        return colour;
    QColor c = ui::readableOn(colour, {base}, minimum);
    if (ui::contrastRatio(c, base) >= minimum)
        return c;
    // ui::readableOn goes by the base's lightness; a mid grey may need the other way.
    const QColor target = ui::contrastRatio(QColor(Qt::white), base) >= ui::contrastRatio(QColor(Qt::black), base) ? QColor(Qt::white) : QColor(Qt::black);
    c                   = colour;
    c.setAlpha(255);
    for (int step = 0; step < 100 && ui::contrastRatio(c, base) < minimum; ++step)
        c = QColor::fromRgbF(c.redF() + (target.redF() - c.redF()) * 0.05, c.greenF() + (target.greenF() - c.greenF()) * 0.05,
                             c.blueF() + (target.blueF() - c.blueF()) * 0.05);
    return ui::contrastRatio(c, base) >= minimum ? c : target;
}

QColor presetTone(const QString& choice, bool dark)
{
    if (choice == QLatin1String("text"))
        return {};
    const Tones t = tonesOf(choice);
    return QColor(QLatin1String(dark ? t.dark : t.light));
}

void Cache::read(const QPalette& chatTabPalette)
{
    fromPalette = chatcolors::fromPalette(chatTabPalette);
    texture     = isTexture(chatTabPalette);
    colors      = haveSample ? withSample(fromPalette, sampled, texture) : fromPalette;
    valid       = true;
}

void Cache::invalidate()
{
    valid      = false;
    sampleDone = false;
}

bool Cache::takeSample(const QColor& sample)
{
    const Colors before = colors;
    sampled             = sample;
    haveSample          = true;
    sampleDone          = true;
    colors              = withSample(fromPalette, sampled, texture);
    return colors.dark != before.dark || colors.base != before.base;
}

LineColors lineColors(const Colors& chat, const QString& choice)
{
    const QColor base   = chat.base.isValid() ? chat.base : QColor(Qt::white);
    const bool   dark   = chat.base.isValid() ? chat.dark : false;
    const QString which = Settings::normalizeLineColor(choice);
    QColor        tone  = presetTone(which, dark);
    if (which.isEmpty() && chat.themeAccent.isValid()) // 2.7 dark mode: Automatic is the theme's accent
        tone = chat.themeAccent;
    if (!tone.isValid()) // "text": the chat's own text colour
        tone = chat.text.isValid() ? chat.text : (dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black));
    LineColors out;
    out.accent = readable(tone, base).name();
    out.warning = readable(dark ? QColor(0xf0, 0xb2, 0x32) : QColor(0xb5, 0x47, 0x08), base).name();
    if (chat.themeAccent.isValid()) // 2.7 dark mode: the theme's amber
        out.warning = readable(QColor(0xe5, 0xb5, 0x67), base).name();
    // TeamSpeak draws plugin lines in dark blue (#00008b): on a background where that doesn't read, our lines carry the
    // chat's text colour (made readable) themselves.
    out.wrapBody = ui::contrastRatio(QColor(0x00, 0x00, 0x8b), base) < 4.5;
    QColor text  = chat.text.isValid() ? chat.text : (dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black));
    out.body     = readable(text, base).name();
    return out;
}

QColor sampleBackground(QWidget* chatTab)
{
    if (!chatTab || !chatTab->style())
        return {};
    QImage image(kSample, kSample, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter     painter(&image);
        QStyleOption opt;
        opt.initFrom(chatTab);
        // Only the style sheet's background of the ChatTab (the rule that paints the chat), at its top-left corner: no
        // event reaches the widget, nothing of the text.
        chatTab->style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, chatTab);
    }
    return dominant(image);
}

} // namespace chatcolors
