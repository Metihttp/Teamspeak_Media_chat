#include "spoiler.h"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVector>
#include <QtMath>

#include "blurhash.h"
#include "i18n.h"

namespace spoiler {

namespace {

constexpr qreal kPillPadX     = 12.0;
constexpr qreal kPillPadY     = 6.0;
constexpr qreal kPillInset    = 8.0;  // the pill keeps this far from the box's edges
constexpr qreal kEyeOffDisc   = 24.0;
constexpr int   kCoverEnlarge = 4;    // blurredStill() enlarges the tiny copy this much
constexpr int   kHashWidth    = 32;   // BlurHash covers are decoded this wide

QRectF centered(const QSizeF& size, const QRectF& box)
{
    return QRectF(box.center().x() - size.width() / 2.0, box.center().y() - size.height() / 2.0, size.width(), size.height());
}

QRectF coverRect(const QSizeF& content, const QRectF& box)
{
    if (content.isEmpty())
        return box;
    return centered(content.scaled(box.size(), Qt::KeepAspectRatioByExpanding), box);
}

QFont pillFont(const QFont& font)
{
    QFont f = font;
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    return f;
}

QString pillText()
{
    return i18n::t("SPOILER");
}

int markAlpha(const CoverLook& look)
{
    return look.pressed ? kPillPressedAlpha : look.hovered ? kPillHoverAlpha : kPillAlpha;
}

// A small box blur, run twice (close to a Gaussian), with the edges clamped: smooths the corners that
// enlarging a 12 px picture bilinearly leaves. Premultiplied pixels, so averaging them is right.
void boxBlur(QImage& image, int radius)
{
    const int w = image.width();
    const int h = image.height();
    if (w <= 1 && h <= 1)
        return;
    QVector<QRgb> line(qMax(w, h));
    auto blurLine = [&line, radius](int count, auto&& pixel) {
        for (int i = 0; i < count; ++i)
            line[i] = pixel(i);
        for (int i = 0; i < count; ++i) {
            int a = 0, r = 0, g = 0, b = 0;
            for (int k = -radius; k <= radius; ++k) {
                const QRgb c = line[qBound(0, i + k, count - 1)];
                a += qAlpha(c);
                r += qRed(c);
                g += qGreen(c);
                b += qBlue(c);
            }
            const int n = 2 * radius + 1;
            pixel(i) = qRgba(r / n, g / n, b / n, a / n);
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            blurLine(w, [row](int x) -> QRgb& { return row[x]; });
        }
        for (int x = 0; x < w; ++x)
            blurLine(h, [&image, x](int y) -> QRgb& { return reinterpret_cast<QRgb*>(image.scanLine(y))[x]; });
    }
}

// The link's BlurHash at the box's shape: the same soft picture whatever has been downloaded, so the
// cover doesn't change while the files arrive.
QImage hashPicture(const QString& hash, const QSizeF& box)
{
    if (hash.isEmpty() || box.isEmpty())
        return {};
    const qreal aspect = qBound(0.125, box.width() / box.height(), 8.0);
    const int   height = qBound(4, qRound(kHashWidth / aspect), 256);
    return blurhash::decode(hash, QSize(kHashWidth, height));
}

} // namespace

bool appliesTo(MediaKind kind)
{
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

QString label(MediaKind kind)
{
    switch (kind) {
    case MediaKind::AnimatedImage:
        return i18n::t("Spoiler (GIF)");
    case MediaKind::Video:
        return i18n::t("Spoiler (video)");
    default:
        return i18n::t("Spoiler (image)");
    }
}

QImage blurredStill(const QImage& still, bool isBlurHash)
{
    if (still.isNull())
        return {};
    if (isBlurHash)
        return still;
    // Qt shrinks by area averaging, so every pixel of the original counts and none stands out.
    QImage tiny = still.scaled(kCoverDetail, kCoverDetail, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    tiny.setDevicePixelRatio(1.0);
    QImage soft = tiny.scaled(tiny.size() * kCoverEnlarge, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    soft.setDevicePixelRatio(1.0);
    boxBlur(soft, kCoverEnlarge / 2);
    return soft;
}

QImage coverSource(const QString& blurHash, const QSizeF& box, const QImage& still, bool stillIsBlurHash, bool* isBlurHash)
{
    const QImage hash = hashPicture(blurHash, box);
    if (isBlurHash)
        *isBlurHash = !hash.isNull() || stillIsBlurHash;
    return hash.isNull() ? still : hash;
}

QRectF pillRect(const QRectF& bounds, const QFont& font)
{
    const QFontMetricsF fm(pillFont(font));
    const qreal         height = qCeil(fm.height()) + 2.0 * kPillPadY;
    const qreal         width  = qCeil(fm.horizontalAdvance(pillText())) + 2.0 * kPillPadX;
    if (width > bounds.width() - 2.0 * kPillInset || height > bounds.height() - 2.0 * kPillInset)
        return {};
    return centered(QSizeF(width, height), bounds);
}

QRectF eyeOffDiscRect(const QRectF& bounds, const QFont& font)
{
    if (!pillRect(bounds, font).isEmpty())
        return {};
    if (bounds.width() < kEyeOffDisc + kPillInset || bounds.height() < kEyeOffDisc + kPillInset)
        return {};
    return centered(QSizeF(kEyeOffDisc, kEyeOffDisc), bounds);
}

void drawEyeOffIcon(QPainter& p, const QRectF& box, const QColor& color)
{
    auto at = [&box](qreal fx, qreal fy) { return QPointF(box.left() + box.width() * fx, box.top() + box.height() * fy); };
    // Same weight as the other outline icons (previewrenderer.cpp strokeFor).
    const qreal stroke = qMax(1.5, qRound(box.width() / 9.0 * 4.0) / 4.0);
    p.save();
    p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(at(0.06, 0.50));
    eye.cubicTo(at(0.28, 0.16), at(0.72, 0.16), at(0.94, 0.50));
    eye.cubicTo(at(0.72, 0.84), at(0.28, 0.84), at(0.06, 0.50));
    p.drawPath(eye);
    p.drawEllipse(at(0.50, 0.50), box.width() * 0.13, box.height() * 0.13);
    p.drawLine(at(0.14, 0.10), at(0.86, 0.90));
    p.restore();
}

void drawCover(QPainter& p, const QRectF& bounds, const QImage& still, bool stillIsBlurHash, const CoverLook& look)
{
    const qreal opacity = qBound(0.0, look.opacity, 1.0);
    if (opacity <= 0.0 || bounds.isEmpty())
        return;

    if (opacity < 1.0) {
        // Faded as one layer: drawn off screen at full strength, then at the fade's opacity (drawing the
        // picture and the dimming separately would darken what is being revealed too).
        const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        QImage      layer(QSize(qMax(1, qCeil(bounds.width() * dpr)), qMax(1, qCeil(bounds.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        QPainter lp(&layer);
        lp.setRenderHints(p.renderHints());
        CoverLook solid = look;
        solid.opacity   = 1.0;
        drawCover(lp, QRectF(QPointF(0, 0), bounds.size()), still, stillIsBlurHash, solid);
        lp.end();
        p.save();
        p.setOpacity(p.opacity() * opacity);
        p.drawImage(bounds, layer);
        p.restore();
        return;
    }

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage soft = blurredStill(still, stillIsBlurHash);
    if (soft.isNull()) {
        p.fillRect(bounds, look.placeholder);
    } else {
        p.fillRect(bounds, Qt::black); // under the edges of a picture that doesn't quite fill the box
        p.drawImage(coverRect(QSizeF(soft.size()), bounds), soft);
        p.fillRect(bounds, QColor(0, 0, 0, stillIsBlurHash ? kBlurHashDimAlpha : kDimAlpha));
    }

    if (look.withPill) {
        const QRectF pill = pillRect(bounds, look.font);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, markAlpha(look)));
        if (look.pressed) { // pressed: a little smaller, like the other buttons on media
            p.translate(bounds.center());
            p.scale(0.96, 0.96);
            p.translate(-bounds.center());
        }
        if (!pill.isEmpty()) {
            p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p.setFont(pillFont(look.font));
            p.setPen(Qt::white);
            // The letter spacing also follows the last letter: shift by half of it to stay centred.
            p.drawText(pill.translated(0.25, 0), Qt::AlignCenter, pillText());
        } else {
            const QRectF disc = eyeOffDiscRect(bounds, look.font);
            if (!disc.isEmpty()) {
                p.drawEllipse(disc);
                drawEyeOffIcon(p, disc.adjusted(5, 5, -5, -5), Qt::white);
            }
        }
    }
    p.restore();
}

} // namespace spoiler

// ---- SpoilerState ---------------------------------------------------------------------------------

bool SpoilerState::noteSighting(const QString& key, bool spoiler)
{
    if (!spoiler || key.isEmpty() || m_spoilers.contains(key))
        return false;
    m_spoilers.insert(key);
    if (m_shownOpen.contains(key))
        m_revealed.insert(key); // already seen uncovered: it stays that way
    return true;
}

void SpoilerState::noteShownOpen(const QString& key)
{
    if (!key.isEmpty())
        m_shownOpen.insert(key);
}

bool SpoilerState::setRevealed(const QString& key, bool revealed)
{
    if (!m_spoilers.contains(key))
        return false;
    if (revealed)
        return !m_revealed.contains(key) && (m_revealed.insert(key), true);
    m_shownOpen.remove(key); // covered on purpose: a later sighting must not uncover it again
    return m_revealed.remove(key);
}

bool SpoilerState::isHidden(const QString& key, bool showAll) const
{
    return !showAll && m_spoilers.contains(key) && !m_revealed.contains(key);
}

QStringList SpoilerState::spoilers() const
{
    return m_spoilers.values();
}

void SpoilerState::clear()
{
    m_spoilers.clear();
    m_revealed.clear();
    m_shownOpen.clear();
    m_heldDownloads.clear();
}

void SpoilerState::holdDownload(const QString& key)
{
    if (!key.isEmpty())
        m_heldDownloads.insert(key);
}

bool SpoilerState::releaseDownload(const QString& key)
{
    return m_heldDownloads.remove(key);
}

// ---- RevealFades ----------------------------------------------------------------------------------

namespace {
constexpr int    kFadeTickMs = 16;
constexpr int    kIdleTickMs = 250;  // no fade running: only old entries are left to forget
constexpr qint64 kRememberMs = 2000; // revealedWithin() looks back at most this far
} // namespace

RevealFades::RevealFades(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(kFadeTickMs);
    connect(m_timer, &QTimer::timeout, this, &RevealFades::tick);
}

void RevealFades::start(const QString& key, bool fade)
{
    if (key.isEmpty())
        return;
    Fade f;
    f.started  = m_clock.elapsed();
    f.fading   = fade;
    f.finished = !fade;
    m_fades.insert(key, f);
    m_timer->start(fade ? kFadeTickMs : kIdleTickMs);
    emit changed(key);
}

void RevealFades::cancel(const QString& key)
{
    if (m_fades.remove(key) > 0)
        emit changed(key);
}

qreal RevealFades::coverOpacity(const QString& key) const
{
    const auto it = m_fades.constFind(key);
    if (it == m_fades.constEnd() || !it->fading)
        return 0.0;
    const qint64 elapsed = m_clock.elapsed() - it->started;
    if (elapsed >= spoiler::kRevealMs)
        return 0.0;
    const qreal t = qBound(0.0, static_cast<qreal>(elapsed) / spoiler::kRevealMs, 1.0);
    return 1.0 - QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t);
}

bool RevealFades::isFading(const QString& key) const
{
    return coverOpacity(key) > 0.0;
}

bool RevealFades::revealedWithin(const QString& key, qint64 ms) const
{
    const auto it = m_fades.constFind(key);
    return it != m_fades.constEnd() && m_clock.elapsed() - it->started < ms;
}

void RevealFades::tick()
{
    const qint64 now = m_clock.elapsed();
    QStringList  redraw;
    bool         fading = false;
    for (auto it = m_fades.begin(); it != m_fades.end();) {
        const qint64 age = now - it->started;
        if (!it->finished) {
            // Every step, and once more when it is over (even if the timer came late): drawn uncovered.
            redraw.append(it.key());
            it->finished = age >= spoiler::kRevealMs;
            fading       = fading || !it->finished;
        }
        if (it->finished && age > kRememberMs)
            it = m_fades.erase(it);
        else
            ++it;
    }
    if (m_fades.isEmpty())
        m_timer->stop();
    else if (m_timer->interval() != (fading ? kFadeTickMs : kIdleTickMs))
        m_timer->start(fading ? kFadeTickMs : kIdleTickMs);
    for (const QString& key : qAsConst(redraw))
        emit changed(key);
}
