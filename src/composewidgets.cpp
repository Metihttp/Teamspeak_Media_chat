#include "composewidgets.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include <cmath>

#include "uiutil.h"

#include <windows.h>

namespace composeui {

namespace {

constexpr int   kRing        = 0;  // room around a control's shape (the focus ring is drawn just inside it)
constexpr int   kButtonH     = 36; // Send, Cancel, Add files
constexpr int   kChipH       = 28;
constexpr qreal kButtonR     = 8.0;

// "Re&vert" -> "Revert", "&&" -> "&": what is painted (mnemonics still work through the button text).
QString plainText(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&') && i + 1 < text.size()) {
            ++i; // the next character is shown as it is ("&&" -> "&")
        }
        out += text.at(i);
    }
    return out;
}

QPen strokePen(const QColor& color, qreal side, qreal factor = 0.09)
{
    return QPen(color, qMax(1.4, side * factor), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

bool isDarkColor(const QColor& c)
{
    return c.lightness() < 128;
}

} // namespace

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(qBound(0.0, alpha, 1.0));
    return color;
}

QColor mixColors(const QColor& from, const QColor& to, qreal amount)
{
    const qreal t = qBound(0.0, amount, 1.0);
    const QColor a = from.toRgb();
    const QColor b = to.toRgb();
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

Theme themeFor(const QPalette& pal)
{
    Theme t;
    t.window = pal.color(QPalette::Active, QPalette::Window);
    t.text   = pal.color(QPalette::Active, QPalette::WindowText);
    t.dark   = isDarkColor(t.window);
    const QColor black(Qt::black);
    const QColor white(Qt::white);
    if (t.dark) {
        t.body        = t.window;
        t.footer      = mixColors(t.window, black, 0.14);
        t.surface     = mixColors(t.window, black, 0.24);
        t.field       = mixColors(t.window, black, 0.20);
        t.fieldBorder = mixColors(t.window, black, 0.38);
        t.chip        = mixColors(t.window, white, 0.07);
        t.chipHover   = mixColors(t.window, white, 0.13);
        t.chipPressed = mixColors(t.window, white, 0.04);
        t.border      = mixColors(t.window, white, 0.08);
    } else {
        // The window's own colour stays the background: skins that paint widgets by style sheet match it.
        t.body        = t.window;
        t.footer      = mixColors(t.window, t.text, 0.045);
        t.surface     = mixColors(t.window, white, 0.72);
        t.field       = white;
        t.fieldBorder = mixColors(t.window, t.text, 0.22);
        t.chip        = mixColors(t.window, t.text, 0.07);
        t.chipHover   = mixColors(t.window, t.text, 0.115);
        t.chipPressed = mixColors(t.window, t.text, 0.16);
        t.border      = mixColors(t.window, t.text, 0.11);
    }
    t.muted = mixColors(t.body, t.text, 0.70);
    for (qreal a = 0.75; ui::contrastRatio(t.muted, t.body) < 4.5 && a <= 1.0; a += 0.05)
        t.muted = mixColors(t.body, t.text, a);
    t.accent = t.dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
    if (ui::contrastRatio(t.accent, t.body) < 3.0)
        t.accent = t.text;
    t.primary        = QColor(0x58, 0x65, 0xf2); // white text 4.6:1
    t.primaryHover   = QColor(0x47, 0x52, 0xc4);
    t.primaryPressed = QColor(0x3c, 0x45, 0xa5);
    t.onPrimary      = white;
    t.error          = t.dark ? QColor(0xff, 0x8a, 0x8f) : QColor(0xc4, 0x28, 0x2d); // 5.1:1 on TeamSpeak's dark grey
    if (ui::contrastRatio(t.error, t.body) < 4.5)
        t.error = t.text; // the alert glyph still marks it
    t.errorTint = mixColors(t.body, t.error, t.dark ? 0.14 : 0.08);
    t.scrim     = QColor(0, 0, 0, 150);
    return t;
}

// ---- glyphs -----------------------------------------------------------------------------------------

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    const qreal   s  = box.width();
    const QPointF c  = box.center();
    const auto    at = [&box, s](qreal x, qreal y) { return QPointF(box.left() + x * s, box.top() + y * s); };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(strokePen(color, s));
    p.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::None:
        break;
    case Glyph::Spoiler: {
        QPainterPath eye;
        eye.moveTo(c.x() - s * 0.44, c.y());
        eye.quadTo(c.x(), c.y() - s * 0.46, c.x() + s * 0.44, c.y());
        eye.quadTo(c.x(), c.y() + s * 0.46, c.x() - s * 0.44, c.y());
        p.drawPath(eye);
        p.drawEllipse(c, s * 0.12, s * 0.12);
        p.drawLine(at(0.12, 0.12), at(0.88, 0.88));
        break;
    }
    case Glyph::Remove: {
        const qreal h = s * 0.28;
        p.setPen(strokePen(color, s, 0.1));
        p.drawLine(c + QPointF(-h, -h), c + QPointF(h, h));
        p.drawLine(c + QPointF(h, -h), c + QPointF(-h, h));
        break;
    }
    case Glyph::Edit: {
        const QPointF tip  = at(0.14, 0.86);
        const QPointF end  = at(0.84, 0.16);
        const QPointF d    = (end - tip) / std::hypot(end.x() - tip.x(), end.y() - tip.y());
        const QPointF n(-d.y(), d.x());
        const qreal   w    = s * 0.13;
        const QPointF neck = tip + d * (s * 0.24);
        QPolygonF     body;
        body << tip << neck + n * w << end + n * w << end - n * w << neck - n * w;
        p.drawPolygon(body);
        p.drawLine(neck + n * w, neck - n * w);
        break;
    }
    case Glyph::Revert: {
        QPainterPath path;
        path.moveTo(at(0.20, 0.38));
        path.lineTo(at(0.60, 0.38));
        path.cubicTo(at(0.92, 0.38), at(0.92, 0.84), at(0.60, 0.84));
        path.lineTo(at(0.36, 0.84));
        p.drawPath(path);
        p.drawLine(at(0.20, 0.38), at(0.38, 0.20));
        p.drawLine(at(0.20, 0.38), at(0.38, 0.56));
        break;
    }
    case Glyph::Plus:
        p.setPen(strokePen(color, s, 0.1));
        p.drawLine(at(0.5, 0.18), at(0.5, 0.82));
        p.drawLine(at(0.18, 0.5), at(0.82, 0.5));
        break;
    case Glyph::Upload: {
        p.drawLine(at(0.5, 0.14), at(0.5, 0.62));
        p.drawLine(at(0.5, 0.14), at(0.30, 0.34));
        p.drawLine(at(0.5, 0.14), at(0.70, 0.34));
        QPainterPath tray;
        tray.moveTo(at(0.14, 0.58));
        tray.lineTo(at(0.14, 0.80));
        tray.quadTo(at(0.14, 0.86), at(0.20, 0.86));
        tray.lineTo(at(0.80, 0.86));
        tray.quadTo(at(0.86, 0.86), at(0.86, 0.80));
        tray.lineTo(at(0.86, 0.58));
        p.drawPath(tray);
        break;
    }
    case Glyph::Channel:
        p.drawLine(at(0.40, 0.14), at(0.32, 0.86));
        p.drawLine(at(0.68, 0.14), at(0.60, 0.86));
        p.drawLine(at(0.18, 0.36), at(0.86, 0.36));
        p.drawLine(at(0.14, 0.64), at(0.82, 0.64));
        break;
    case Glyph::Person: {
        p.drawEllipse(QPointF(c.x(), box.top() + s * 0.34), s * 0.17, s * 0.17);
        QPainterPath body;
        body.moveTo(at(0.18, 0.86));
        body.cubicTo(at(0.20, 0.60), at(0.80, 0.60), at(0.82, 0.86));
        p.drawPath(body);
        break;
    }
    case Glyph::Server: {
        p.drawEllipse(c, s * 0.38, s * 0.38);
        p.drawEllipse(c, s * 0.16, s * 0.38);
        p.drawLine(QPointF(c.x() - s * 0.38, c.y()), QPointF(c.x() + s * 0.38, c.y()));
        break;
    }
    case Glyph::Play: {
        QPolygonF tri;
        tri << at(0.32, 0.20) << at(0.82, 0.50) << at(0.32, 0.80);
        p.setPen(QPen(color, qMax(1.0, s * 0.06), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPolygon(tri);
        break;
    }
    case Glyph::Quality: {
        const qreal ys[3] = {0.24, 0.5, 0.76};
        const qreal xs[3] = {0.66, 0.34, 0.58};
        for (int i = 0; i < 3; ++i) {
            p.drawLine(at(0.12, ys[i]), at(0.88, ys[i]));
            p.save();
            p.setBrush(color);
            p.drawEllipse(at(xs[i], ys[i]), s * 0.09, s * 0.09);
            p.restore();
        }
        break;
    }
    case Glyph::Chevron:
        p.setPen(strokePen(color, s, 0.12));
        p.drawLine(at(0.22, 0.38), at(0.5, 0.66));
        p.drawLine(at(0.5, 0.66), at(0.78, 0.38));
        break;
    case Glyph::Album: {
        const qreal g = s * 0.08;
        const qreal w = (s * 0.72 - g) / 2;
        for (int i = 0; i < 4; ++i) {
            const QRectF r(box.left() + s * 0.14 + (i % 2) * (w + g), box.top() + s * 0.14 + (i / 2) * (w + g), w, w);
            p.drawRoundedRect(r, s * 0.06, s * 0.06);
        }
        break;
    }
    case Glyph::Check:
        p.setPen(strokePen(color, s, 0.12));
        p.drawLine(at(0.18, 0.52), at(0.42, 0.74));
        p.drawLine(at(0.42, 0.74), at(0.84, 0.28));
        break;
    }
    p.restore();
}

void drawAlert(QPainter& p, const QRectF& box, const QColor& fill, const QColor& mark)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(box.adjusted(s * 0.04, s * 0.04, -s * 0.04, -s * 0.04));
    p.setPen(QPen(mark, s * 0.12, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx, box.top() + s * 0.26), QPointF(cx, box.top() + s * 0.56));
    p.setPen(Qt::NoPen);
    p.setBrush(mark);
    p.drawEllipse(QPointF(cx, box.top() + s * 0.74), s * 0.07, s * 0.07);
    p.restore();
}

QColor fileTint(const QString& extension, bool dark)
{
    const QString e = extension.toLower();
    const auto    in = [&e](std::initializer_list<const char*> list) {
        for (const char* x : list) {
            if (e == QLatin1String(x))
                return true;
        }
        return false;
    };
    QColor tint(0x6b, 0x72, 0x80); // other
    if (in({"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst", "cab", "iso"}))
        tint = QColor(0xb7, 0x79, 0x1f);
    else if (in({"pdf"}))
        tint = QColor(0xc9, 0x3c, 0x37);
    else if (in({"doc", "docx", "odt", "rtf", "txt", "md", "log", "pages"}))
        tint = QColor(0x2f, 0x6f, 0xdb);
    else if (in({"xls", "xlsx", "csv", "ods", "numbers", "tsv"}))
        tint = QColor(0x1f, 0x8a, 0x52);
    else if (in({"ppt", "pptx", "odp", "key"}))
        tint = QColor(0xc2, 0x62, 0x1d);
    else if (in({"mp3", "wav", "ogg", "flac", "m4a", "aac", "opus", "wma"}))
        tint = QColor(0xb8, 0x3f, 0x84);
    else if (in({"mp4", "webm", "mkv", "mov", "avi", "wmv", "m4v", "3gp", "mts", "m2ts", "mpg", "mpeg"}))
        tint = QColor(0x70, 0x48, 0xe8);
    else if (in({"png", "jpg", "jpeg", "jfif", "gif", "webp", "bmp", "tif", "tiff", "heic", "svg", "ico"}))
        tint = QColor(0x0f, 0x85, 0x77);
    else if (in({"exe", "msi", "dll", "bat", "cmd", "ps1", "sh", "apk"}))
        tint = QColor(0x52, 0x60, 0x6d);
    return dark ? tint.lighter(108) : tint;
}

void drawFileArt(QPainter& p, const QRectF& box, const QString& extension, const QColor& tint, const Theme& theme)
{
    // A page about 5:6, centred in box.
    const qreal h    = box.height();
    const qreal w    = qMin(box.width(), h * 0.82);
    const QRectF page(box.center().x() - w / 2, box.top(), w, h);
    const qreal fold = w * 0.30;
    const qreal r    = w * 0.10;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath sheet;
    sheet.moveTo(page.left() + r, page.top());
    sheet.lineTo(page.right() - fold, page.top());
    sheet.lineTo(page.right(), page.top() + fold);
    sheet.lineTo(page.right(), page.bottom() - r);
    sheet.quadTo(page.right(), page.bottom(), page.right() - r, page.bottom());
    sheet.lineTo(page.left() + r, page.bottom());
    sheet.quadTo(page.left(), page.bottom(), page.left(), page.bottom() - r);
    sheet.lineTo(page.left(), page.top() + r);
    sheet.quadTo(page.left(), page.top(), page.left() + r, page.top());
    const QColor paper = theme.dark ? mixColors(theme.window, Qt::white, 0.16) : QColor(Qt::white);
    p.setPen(QPen(theme.dark ? mixColors(theme.window, Qt::white, 0.24) : mixColors(theme.body, theme.text, 0.18), qMax(1.0, w * 0.02)));
    p.setBrush(paper);
    p.drawPath(sheet);
    QPainterPath corner;
    corner.moveTo(page.right() - fold, page.top());
    corner.lineTo(page.right() - fold, page.top() + fold - r * 0.6);
    corner.quadTo(page.right() - fold, page.top() + fold, page.right() - fold + r * 0.6, page.top() + fold);
    corner.lineTo(page.right(), page.top() + fold);
    corner.closeSubpath();
    p.setBrush(mixColors(paper, tint, 0.35));
    p.drawPath(corner);
    // Lines of "text" on the page.
    p.setPen(QPen(mixColors(paper, theme.text, 0.14), qMax(1.0, h * 0.035), Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 2; ++i) {
        const qreal y = page.top() + h * (0.30 + i * 0.12);
        p.drawLine(QPointF(page.left() + w * 0.18, y), QPointF(page.right() - w * (i == 0 ? 0.40 : 0.22), y));
    }
    // The extension on a band.
    const QString label = extension.left(4).toUpper();
    if (!label.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(qMax(7, qRound(h * 0.19)));
        font.setBold(true);
        const QFontMetricsF fm(font);
        const qreal  bandW = qMin(box.width(), qMax(w * 0.78, fm.horizontalAdvance(label) + h * 0.16));
        const QRectF band(page.left() - (bandW - w * 0.78) / 2 - w * 0.06, page.top() + h * 0.56, bandW, h * 0.27);
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawRoundedRect(band, h * 0.05, h * 0.05);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(band, Qt::AlignCenter, label);
    }
    p.restore();
}

void drawFocusRing(QPainter& p, const QRectF& shape, qreal radius, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(shape.adjusted(1, 1, -1, -1), qMax(0.0, radius - 1), qMax(0.0, radius - 1));
    p.restore();
}

// ---- Fader -------------------------------------------------------------------------------------------

Fader::Fader(QWidget* target, int durationMs)
    : QObject(target)
    , m_target(target)
{
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(durationMs);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_value = value.toReal();
        m_target->update();
    });
}

void Fader::setTarget(qreal goal)
{
    if (qFuzzyCompare(goal + 1.0, m_goal + 1.0))
        return;
    m_goal = goal;
    m_anim->stop();
    if (!m_target->isVisible() || !ui::animationsEnabled()) {
        m_value = goal;
        m_target->update();
        return;
    }
    m_anim->setStartValue(m_value);
    m_anim->setEndValue(goal);
    m_anim->start();
}

void Fader::jumpTo(qreal value)
{
    m_anim->stop();
    m_goal  = value;
    m_value = value;
    m_target->update();
}

bool FocusTracker::byKeyboard(QFocusEvent* event, const QWidget* widget)
{
    switch (event->reason()) {
    case Qt::TabFocusReason:
    case Qt::BacktabFocusReason:
    case Qt::ShortcutFocusReason:
        return true;
    case Qt::OtherFocusReason: // the window moved it (after Delete, the editor closed): a ring once keys were used
    // 2.3 compose-v2 review: back from another window (Alt+Tab) or from the Quality menu: the ring stays
    // when it was showing, as Windows keeps its focus rectangles.
    case Qt::ActiveWindowFocusReason:
    case Qt::PopupFocusReason:
        return widget && widget->window() && widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
    default:
        return false;
    }
}

void FocusTracker::onFocusIn(QFocusEvent* event, const QWidget* widget)
{
    m_keyboard = byKeyboard(event, widget);
}

// ---- Button ------------------------------------------------------------------------------------------

Button::Button(Kind kind, const QString& text, QWidget* parent)
    : QPushButton(text, parent)
    , m_kind(kind)
{
    m_hover = new Fader(this);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    // fromLatin1: style sheets keep their text after the plugin is unloaded.
    setProperty("tsmFlat", true);
    m_theme = themeFor(palette());
}

void Button::setGlyph(Glyph glyph)
{
    m_glyph = glyph;
    updateGeometry();
    update();
}

void Button::setTheme(const Theme& theme)
{
    m_theme = theme;
    update();
}

QFont Button::paintFont() const
{
    QFont f = font();
    f.setBold(false);
    f.setWeight(QFont::DemiBold);
    return f;
}

QSize Button::sizeHint() const
{
    const QFontMetrics fm(paintFont());
    int w = fm.horizontalAdvance(plainText(text())) + 2 * 16;
    if (m_glyph != Glyph::None)
        w += 16 + 8 - 4;
    w = qMax(w, m_kind == Kind::Ghost ? 0 : 88);
    return QSize(w + 2 * kRing, kButtonH + 2 * kRing);
}

QSize Button::minimumSizeHint() const
{
    return sizeHint();
}

void Button::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF shape = QRectF(rect()).adjusted(kRing, kRing, -kRing, -kRing);
    const qreal  hover = m_hover->value();
    QColor       fill;
    QColor       ink;
    switch (m_kind) {
    case Kind::Primary:
        fill = isDown() ? m_theme.primaryPressed : mixColors(m_theme.primary, m_theme.primaryHover, hover);
        ink  = m_theme.onPrimary;
        if (!isEnabled()) {
            fill = m_theme.chip;
            ink  = withAlpha(m_theme.muted, 0.75);
        }
        break;
    case Kind::Secondary:
        fill = isDown() ? m_theme.chipPressed : mixColors(m_theme.chip, m_theme.chipHover, hover);
        ink  = isEnabled() ? m_theme.text : withAlpha(m_theme.muted, 0.6);
        break;
    case Kind::Ghost:
        fill = isDown() ? m_theme.chipPressed : withAlpha(m_theme.chipHover, hover);
        ink  = isEnabled() ? m_theme.text : withAlpha(m_theme.muted, 0.6);
        break;
    }
    if (fill.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(shape, kButtonR, kButtonR);
    }
    QRectF content = shape.adjusted(16, 0, -16, 0);
    p.setFont(paintFont());
    const QString label = plainText(text());
    if (m_glyph != Glyph::None) {
        const qreal textW = QFontMetricsF(p.font()).horizontalAdvance(label);
        const qreal total = 16 + 8 + textW;
        const qreal left  = shape.center().x() - total / 2;
        drawGlyph(p, m_glyph, QRectF(left, shape.center().y() - 8, 16, 16), m_kind == Kind::Primary ? ink : (isEnabled() ? m_theme.accent : ink));
        content = QRectF(left + 24, shape.top(), textW + 1, shape.height());
        p.setPen(ink);
        p.drawText(content, Qt::AlignLeft | Qt::AlignVCenter, label);
    } else {
        p.setPen(ink);
        p.drawText(shape, Qt::AlignCenter, label);
    }
    if (hasFocus() && m_focus.keyboard())
        drawFocusRing(p, shape, kButtonR, m_kind == Kind::Primary && isEnabled() ? m_theme.onPrimary : m_theme.accent);
}

void Button::enterEvent(QEvent* event)
{
    QPushButton::enterEvent(event);
    m_hover->setTarget(1.0);
}

void Button::leaveEvent(QEvent* event)
{
    QPushButton::leaveEvent(event);
    m_hover->setTarget(0.0);
}

void Button::focusInEvent(QFocusEvent* event)
{
    m_focus.onFocusIn(event, this);
    QPushButton::focusInEvent(event);
}

void Button::changeEvent(QEvent* event)
{
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateGeometry();
    if (event->type() == QEvent::EnabledChange && !isEnabled())
        m_hover->jumpTo(0.0);
}

// ---- Chip --------------------------------------------------------------------------------------------

Chip::Chip(Glyph glyph, const QString& text, QWidget* parent)
    : QAbstractButton(parent)
    , m_glyph(glyph)
{
    setText(text);
    m_hover = new Fader(this);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setProperty("tsmFlat", true);
    m_theme = themeFor(palette());
}

void Chip::setTheme(const Theme& theme)
{
    m_theme = theme;
    update();
}

void Chip::setMenuIndicator(bool on)
{
    m_menu = on;
    updateGeometry();
    update();
}

void Chip::setMaximumTextWidth(int width)
{
    m_maxText = width;
    updateGeometry();
}

QSize Chip::sizeHint() const
{
    const QFontMetrics fm(font());
    int textW = fm.horizontalAdvance(plainText(text()));
    if (m_maxText > 0)
        textW = qMin(textW, m_maxText);
    int w = 12 + textW + 12;
    if (m_glyph != Glyph::None)
        w += 14 + 6 - 2;
    if (m_menu)
        w += 4 + 10;
    return QSize(w + 2 * kRing, kChipH + 2 * kRing);
}

QSize Chip::minimumSizeHint() const
{
    const QSize hint = sizeHint();
    return QSize(qMin(hint.width(), 64 + 2 * kRing), hint.height());
}

void Chip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF shape  = QRectF(rect()).adjusted(kRing, kRing, -kRing, -kRing);
    const qreal  radius = shape.height() / 2;
    QColor       fill   = isDown() ? m_theme.chipPressed : mixColors(m_theme.chip, m_theme.chipHover, m_hover->value());
    QColor       ink    = m_theme.text;
    QColor       mark   = m_theme.muted;
    if (isChecked()) {
        fill = mixColors(m_theme.body, m_theme.accent, isDown() ? 0.30 : 0.18 + 0.06 * m_hover->value());
        ink  = m_theme.text;
        mark = m_theme.accent;
    }
    if (!isEnabled()) {
        fill = withAlpha(m_theme.chip, 0.6);
        ink  = withAlpha(m_theme.muted, 0.8);
        mark = withAlpha(m_theme.muted, 0.6);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(shape, radius, radius);
    qreal x = shape.left() + 12;
    if (m_glyph != Glyph::None) {
        drawGlyph(p, m_glyph, QRectF(x - 2, shape.center().y() - 7, 14, 14), isEnabled() && !isChecked() ? m_theme.accent : mark);
        x += 14 + 6 - 2;
    }
    qreal right = shape.right() - 12;
    if (m_menu) {
        drawGlyph(p, Glyph::Chevron, QRectF(right - 10, shape.center().y() - 5, 10, 10), mark);
        right -= 10 + 4;
    }
    p.setPen(ink);
    p.setFont(font());
    const QFontMetricsF fm(font());
    const QString       label = fm.elidedText(plainText(text()), Qt::ElideRight, qMax(0.0, right - x));
    p.drawText(QRectF(x, shape.top(), qMax(0.0, right - x), shape.height()), Qt::AlignLeft | Qt::AlignVCenter, label);
    if (hasFocus() && m_focus.keyboard())
        drawFocusRing(p, shape, radius, m_theme.accent);
}

void Chip::enterEvent(QEvent* event)
{
    QAbstractButton::enterEvent(event);
    m_hover->setTarget(1.0);
}

void Chip::leaveEvent(QEvent* event)
{
    QAbstractButton::leaveEvent(event);
    m_hover->setTarget(0.0);
}

void Chip::focusInEvent(QFocusEvent* event)
{
    m_focus.onFocusIn(event, this);
    QAbstractButton::focusInEvent(event);
}

void Chip::keyPressEvent(QKeyEvent* event)
{
    const bool plain = (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if (plain && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        click(); // like a focused push button: Enter acts on it, it doesn't send
        return;
    }
    if (m_step && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && plain) {
        m_step(event->key() == Qt::Key_Up ? -1 : 1);
        return;
    }
    if (m_step && event->key() == Qt::Key_Down && event->modifiers() == Qt::AltModifier) {
        click();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

// ---- IconButton --------------------------------------------------------------------------------------

IconButton::IconButton(Glyph glyph, Look look, int side, QWidget* parent)
    : QAbstractButton(parent)
    , m_glyph(glyph)
    , m_look(look)
    , m_side(side)
{
    m_hover = new Fader(this);
    setFixedSize(side, side);
    // Tab focus only: a click doesn't take the focus away from the caption.
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setProperty("tsmFlat", true);
    m_theme = themeFor(palette());
}

void IconButton::setTheme(const Theme& theme)
{
    m_theme = theme;
    update();
}

QSize IconButton::sizeHint() const
{
    return QSize(m_side, m_side);
}

void IconButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal reveal = m_reveal ? m_reveal->value() : 1.0;
    const bool  keyRing = hasFocus() && m_focus.keyboard();
    if (reveal <= 0.001 && !keyRing && !isChecked())
        return;
    // A checked spoiler toggle stays visible on the picture: it says how the picture goes out.
    p.setOpacity(keyRing || isChecked() ? 1.0 : reveal);
    const QRectF shape  = QRectF(rect()).adjusted(2, 2, -2, -2);
    const qreal  radius = m_look == Look::OnMedia ? shape.height() / 2 : 8.0;
    const qreal  hover  = m_hover->value();
    QColor       fill(Qt::transparent);
    QColor       glyph;
    if (m_look == Look::OnMedia) {
        if (isChecked())
            fill = isDown() ? m_theme.primaryPressed : mixColors(m_theme.primary, m_theme.primaryHover, hover);
        else
            fill = withAlpha(Qt::white, isDown() ? 0.30 : 0.20 * hover);
        glyph = Qt::white;
        if (!isEnabled())
            glyph = withAlpha(Qt::white, 0.4);
    } else {
        if (isChecked())
            fill = mixColors(m_theme.body, m_theme.accent, isDown() ? 0.30 : 0.18);
        else if (isDown())
            fill = m_theme.chipPressed;
        else
            fill = withAlpha(m_theme.chipHover, hover);
        glyph = !isEnabled() ? withAlpha(m_theme.muted, 0.4) : isChecked() ? m_theme.accent : mixColors(m_theme.muted, m_theme.text, hover);
    }
    if (fill.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(shape, radius, radius);
    }
    const qreal side = qRound(m_side * 0.5);
    QRectF      box(0, 0, side, side);
    box.moveCenter(QRectF(rect()).center());
    drawGlyph(p, m_glyph, box, glyph);
    if (keyRing) {
        p.setOpacity(1.0);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(m_look == Look::OnMedia ? QColor(Qt::white) : m_theme.accent, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius + 1, radius + 1);
    }
}

void IconButton::enterEvent(QEvent* event)
{
    QAbstractButton::enterEvent(event);
    m_hover->setTarget(1.0);
}

void IconButton::leaveEvent(QEvent* event)
{
    QAbstractButton::leaveEvent(event);
    m_hover->setTarget(0.0);
}

void IconButton::focusInEvent(QFocusEvent* event)
{
    m_focus.onFocusIn(event, this);
    QAbstractButton::focusInEvent(event);
}

void IconButton::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        click(); // like a focused push button: Enter acts on it, it doesn't send
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

// ---- SwitchRow ---------------------------------------------------------------------------------------

namespace {
constexpr int kSwitchW   = 36;
constexpr int kSwitchH   = 20;
constexpr int kRowPadX   = 12;
constexpr int kRowPadY   = 10;
constexpr int kRowGlyph  = 18;
} // namespace

SwitchRow::SwitchRow(Glyph glyph, const QString& title, QWidget* parent)
    : QAbstractButton(parent)
    , m_glyph(glyph)
{
    setText(title);
    setCheckable(true);
    m_hover = new Fader(this);
    m_knob  = new Fader(this, 160);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setProperty("tsmFlat", true);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    m_theme = themeFor(palette());
    connect(this, &QAbstractButton::toggled, this, [this](bool on) { m_knob->setTarget(on ? 1.0 : 0.0); });
}

void SwitchRow::setTheme(const Theme& theme)
{
    m_theme = theme;
    if (!qFuzzyCompare(m_knob->target() + 1.0, (isChecked() ? 1.0 : 0.0) + 1.0))
        m_knob->jumpTo(isChecked() ? 1.0 : 0.0);
    update();
}

void SwitchRow::setSubtitle(const QString& subtitle)
{
    if (subtitle == m_subtitle)
        return;
    m_subtitle = subtitle;
    setAccessibleDescription(subtitle);
    updateGeometry();
    update();
}

int SwitchRow::textWidth(int width) const
{
    int left = kRowPadX + (m_glyph != Glyph::None ? kRowGlyph + 12 : 0);
    return qMax(40, width - left - 16 - kSwitchW - kRowPadX);
}

int SwitchRow::heightForWidth(int width) const
{
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics title(bold);
    const QFontMetrics sub(font());
    int h = title.height();
    if (!m_subtitle.isEmpty())
        h += 2 + sub.boundingRect(QRect(0, 0, textWidth(width), 4096), Qt::TextWordWrap, m_subtitle).height();
    return qMax(h, kSwitchH) + 2 * kRowPadY;
}

QSize SwitchRow::sizeHint() const
{
    const int w = qMax(width(), 320);
    return QSize(w, heightForWidth(w));
}

QSize SwitchRow::minimumSizeHint() const
{
    return QSize(160, heightForWidth(qMax(width(), 160)));
}

QRectF SwitchRow::switchRect() const
{
    return QRectF(width() - kRowPadX - kSwitchW, (height() - kSwitchH) / 2.0, kSwitchW, kSwitchH);
}

void SwitchRow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF row = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal  hover = m_hover->value();
    if (hover > 0.0 || isDown()) {
        p.setPen(Qt::NoPen);
        p.setBrush(isDown() ? m_theme.chip : withAlpha(m_theme.chip, hover));
        p.drawRoundedRect(row, 10, 10);
    }
    qreal x = kRowPadX;
    if (m_glyph != Glyph::None) {
        drawGlyph(p, m_glyph, QRectF(x, (height() - kRowGlyph) / 2.0, kRowGlyph, kRowGlyph), isEnabled() ? m_theme.accent : m_theme.muted);
        x += kRowGlyph + 12;
    }
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics titleFm(bold);
    const QFontMetrics subFm(font());
    const int          tw     = textWidth(width());
    int                blockH = titleFm.height();
    QRect              subRect;
    if (!m_subtitle.isEmpty()) {
        subRect = subFm.boundingRect(QRect(0, 0, tw, 4096), Qt::TextWordWrap, m_subtitle);
        blockH += 2 + subRect.height();
    }
    int y = (height() - blockH) / 2;
    p.setFont(bold);
    p.setPen(isEnabled() ? m_theme.text : m_theme.muted);
    p.drawText(QRect(int(x), y, tw, titleFm.height()), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic, text());
    if (!m_subtitle.isEmpty()) {
        p.setFont(font());
        p.setPen(m_theme.muted);
        p.drawText(QRect(int(x), y + titleFm.height() + 2, tw, subRect.height()), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_subtitle);
    }
    // The switch.
    const QRectF track = switchRect();
    const qreal  on    = m_knob->value();
    const QColor off   = m_theme.dark ? mixColors(m_theme.body, m_theme.text, 0.30) : mixColors(m_theme.body, m_theme.text, 0.38);
    p.setPen(Qt::NoPen);
    p.setBrush(isEnabled() ? mixColors(off, m_theme.primary, on) : withAlpha(off, 0.5));
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    const qreal  knobD = track.height() - 6;
    const QPointF knob(track.left() + 3 + knobD / 2 + on * (track.width() - 6 - knobD), track.center().y());
    p.setBrush(Qt::white);
    p.drawEllipse(knob, knobD / 2, knobD / 2);
    if (hasFocus() && m_focus.keyboard())
        drawFocusRing(p, row, 10, m_theme.accent);
}

void SwitchRow::enterEvent(QEvent* event)
{
    QAbstractButton::enterEvent(event);
    m_hover->setTarget(1.0);
}

void SwitchRow::leaveEvent(QEvent* event)
{
    QAbstractButton::leaveEvent(event);
    m_hover->setTarget(0.0);
}

void SwitchRow::focusInEvent(QFocusEvent* event)
{
    m_focus.onFocusIn(event, this);
    QAbstractButton::focusInEvent(event);
}

// ---- the title bar -----------------------------------------------------------------------------------

void applyWindowCaption(QWidget* window, const Theme& theme)
{
    if (!window || !window->isWindow() || !window->testAttribute(Qt::WA_WState_Created) || QGuiApplication::platformName() != QLatin1String("windows"))
        return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;
    // dwmapi is in every desktop process already; looked up at run time so the plugin imports nothing new.
    HMODULE dwm    = GetModuleHandleW(L"dwmapi.dll");
    bool    loaded = false;
    if (!dwm) {
        dwm    = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        loaded = dwm != nullptr;
    }
    if (!dwm)
        return;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto set     = reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (set) {
        const BOOL dark = theme.dark ? TRUE : FALSE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 (Windows 10 20H1 and later), 19 before.
        if (FAILED(set(hwnd, 20, &dark, sizeof(dark))))
            set(hwnd, 19, &dark, sizeof(dark));
        // Windows 11: DWMWA_CAPTION_COLOR (35) and DWMWA_TEXT_COLOR (36); earlier versions refuse them.
        const COLORREF caption = RGB(theme.body.red(), theme.body.green(), theme.body.blue());
        const COLORREF text    = RGB(theme.text.red(), theme.text.green(), theme.text.blue());
        set(hwnd, 35, &caption, sizeof(caption));
        set(hwnd, 36, &text, sizeof(text));
    }
    if (loaded)
        FreeLibrary(dwm);
}

} // namespace composeui
