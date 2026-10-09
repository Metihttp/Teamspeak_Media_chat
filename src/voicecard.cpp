#include "voicecard.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include "audio/waveform.h"
#include "i18n.h"
#include "previewpaint.h"
#include "uiutil.h"

namespace pp = previewpaint;

namespace {

// Layout (logical px) of the 56 px card.
constexpr int   kMaxWidth   = 320;
constexpr int   kMinWidth   = 160;
constexpr int   kHeight     = 56;
constexpr qreal kDisc       = 36.0;
constexpr qreal kDiscX      = 10.0;
constexpr qreal kDiscHitPad = 4.0;
constexpr qreal kWaveLeft   = 56.0;
constexpr qreal kRightPad   = 10.0;
constexpr qreal kTimeWidth  = 40.0;
constexpr qreal kGap        = 8.0;
constexpr qreal kNoTimeBelow = 180.0; // narrower cards drop the time
constexpr qreal kBar        = 3.0;
constexpr qreal kBarGap     = 2.0;
constexpr qreal kBarMin     = 3.0;
constexpr qreal kBarRange   = 21.0; // a level of 15 is 3 + 21 = 24 px tall
constexpr qreal kCenterY    = 28.0;
constexpr qreal kSeekTop    = 6.0;
constexpr qreal kSeekBottom = 50.0;
constexpr qreal kBadge      = 14.0;

// The button's hover and pressed fills (white icons stay above 4.5:1 on all three), as on the audio card.
const QColor kAccentHover(0x47, 0x52, 0xc4);
const QColor kAccentPressed(0x3c, 0x45, 0xa5);

struct WaveColors {
    QColor played;   // 3:1+ on the card, clearly lighter (dark) / darker (light) than unplayed
    QColor unplayed; // 3:1+ on the card
};

WaveColors waveColors(bool dark)
{
    if (dark)
        return {QColor(0xc9, 0xcd, 0xfb), QColor(0x80, 0x84, 0x8e)};
    return {QColor(0x47, 0x52, 0xc4), QColor(0x80, 0x84, 0x8e)};
}

struct VoiceGeometry {
    QRectF card;
    QRectF disc;
    QRectF discHit;
    QRectF wave;    // the bars' area
    QRectF seekHit; // the waveform at its full height
    QRectF time;    // empty when hidden
};

VoiceGeometry geometryFor(const QSizeF& size)
{
    VoiceGeometry g;
    const qreal   w = size.width();
    g.card          = QRectF(QPointF(0, 0), size);
    g.disc          = QRectF(kDiscX, (size.height() - kDisc) / 2.0, kDisc, kDisc);
    g.discHit       = g.disc.adjusted(-kDiscHitPad, -kDiscHitPad, kDiscHitPad, kDiscHitPad);
    qreal right     = w - kRightPad;
    if (w >= kNoTimeBelow) {
        g.time = QRectF(right - kTimeWidth, kCenterY - 10.0, kTimeWidth, 20.0);
        right  = g.time.left() - kGap;
    }
    g.wave    = QRectF(kWaveLeft, kCenterY - (kBarMin + kBarRange) / 2.0, qMax(0.0, right - kWaveLeft), kBarMin + kBarRange);
    g.seekHit = QRectF(g.wave.left() - 4.0, kSeekTop, g.wave.width() + 8.0, kSeekBottom - kSeekTop);
    return g;
}

QFont timeFont(const PreviewStyle& style)
{
    return pp::font(style, -1.5);
}

qint64 durationOf(const MediaEntry& e, const PlaybackOverlay& o)
{
    return o.durationMs > 0 ? o.durationMs : qMax<qint64>(0, e.link.durationMs);
}

// Played or paused part-way (an ended message looks as it did before it was played).
bool isStarted(const PlaybackOverlay& o)
{
    return !o.ended && (o.playing || o.positionMs > 0);
}

bool showsFailure(const MediaEntry& e, const PlaybackOverlay& o)
{
    return e.state == MediaState::Failed && !o.busy && !o.externalOnly;
}

double playedFraction(const PlaybackOverlay& o, qint64 durationMs)
{
    if (!isStarted(o) || durationMs <= 0)
        return 0.0;
    return qBound(0.0, static_cast<double>(o.positionMs) / static_cast<double>(durationMs), 1.0);
}

bool seekable(const MediaEntry& e, const PlaybackOverlay& o)
{
    return !showsFailure(e, o) && !o.externalOnly && durationOf(e, o) > 0;
}

// What the waveform area says instead of bars (an error, can't play here); empty: the bars.
QStringList statusTexts(const MediaEntry& e, const PlaybackOverlay& o)
{
    if (o.externalOnly)
        return {i18n::t("Can't play here · Click to open"), i18n::t("Click to open")};
    if (showsFailure(e, o)) {
        const QString title = downloadErrorTitle(e.error);
        if (isRetryableDownload(e))
            return {i18n::t("%1 · Click to retry").arg(title), title};
        return {title};
    }
    return {};
}

QString timeText(const MediaEntry& e, const PlaybackOverlay& o)
{
    if (e.state == MediaState::Downloading && o.busy)
        return i18n::t("%1%").arg(qRound(qBound(0.0, e.progress, 1.0) * 100.0));
    const qint64 duration = durationOf(e, o);
    if (isStarted(o))
        return formatDuration(qMax<qint64>(0, o.positionMs));
    return duration > 0 ? formatDuration(duration) : QString::fromLatin1("--:--");
}

QRectF badgeRect(const QRectF& disc)
{
    return QRectF(disc.right() - kBadge + 3.0, disc.bottom() - kBadge + 3.0, kBadge, kBadge);
}

void drawButton(QPainter& p, const VoiceGeometry& g, const MediaEntry& e, const PlaybackOverlay& o, const PreviewStyle& style, const pp::Palette& pal,
                const QColor& cardBackground, bool actionable)
{
    const QRectF disc    = g.disc;
    const bool   failed  = showsFailure(e, o);
    const bool   hot     = actionable && (o.hover == VideoZone::PlayPause || o.hover == VideoZone::Body);
    const bool   down    = actionable && (o.pressed == VideoZone::PlayPause || o.pressed == VideoZone::Body);
    const QRectF icon    = disc.adjusted(10.0, 10.0, -10.0, -10.0);
    const QRectF arrowIn = icon.adjusted(-1.5, -1.5, 1.5, 1.5);

    p.save();
    if (down) {
        p.translate(disc.center());
        p.scale(0.96, 0.96);
        p.translate(-disc.center());
    }
    if (failed && !isRetryableDownload(e)) {
        pp::drawAlertIcon(p, disc, pal.errorFill, Qt::white);
        p.restore();
        return;
    }
    if (style.dark)
        p.setPen(QPen(pal.progress, 1.0)); // the tonal edge keeps the outline at 3:1 on the dark card
    else
        p.setPen(Qt::NoPen);
    p.setBrush(down ? kAccentPressed : hot ? kAccentHover : pp::accent());
    p.drawEllipse(style.dark ? disc.adjusted(0.5, 0.5, -0.5, -0.5) : disc);
    if (o.busy) {
        const qreal stroke = 2.5;
        pp::drawRing(p, disc.center(), disc.width() / 2.0 - stroke - 4.5, stroke, o.busyProgress, QColor(255, 255, 255, 70), Qt::white, style.animate);
    } else if (failed) {
        pp::drawCircularArrow(p, arrowIn, Qt::white);
    } else if (o.externalOnly) {
        pp::drawOpenIcon(p, icon, Qt::white);
    } else if (o.playing) {
        pp::drawPauseIcon(p, icon, Qt::white);
    } else {
        pp::drawPlayIcon(p, icon.translated(1.0, 0.0), Qt::white);
    }
    p.restore();

    if (failed) {
        const QRectF badge = badgeRect(disc);
        p.setPen(Qt::NoPen);
        p.setBrush(cardBackground);
        p.drawEllipse(badge.adjusted(-2.0, -2.0, 2.0, 2.0));
        pp::drawAlertIcon(p, badge, pal.errorFill, Qt::white);
    } else if (e.state == MediaState::Idle && !o.busy && !o.externalOnly && !isStarted(o)) {
        // Not downloaded yet (voice messages normally are): pressing play fetches it first.
        const QRectF badge = badgeRect(disc);
        p.setPen(Qt::NoPen);
        p.setBrush(cardBackground);
        p.drawEllipse(badge.adjusted(-2.0, -2.0, 2.0, 2.0));
        p.setBrush(pal.title);
        p.drawEllipse(badge);
        pp::drawDownloadIcon(p, badge.adjusted(3.0, 3.0, -3.0, -3.0), cardBackground);
    }
}

void drawWave(QPainter& p, const VoiceGeometry& g, const MediaEntry& e, const PlaybackOverlay& o, const WaveColors& colors)
{
    const QRectF wave = g.wave;
    if (wave.width() < kBar)
        return;
    const double fraction = playedFraction(o, durationOf(e, o));
    const qreal  split    = wave.left() + wave.width() * fraction;
    p.setPen(Qt::NoPen);

    // Unplayed everywhere, then the played part on top, clipped exactly at the position.
    auto paint = [&](const QColor& color) {
        p.setBrush(color);
        if (e.link.waveform.isEmpty()) {
            // No waveform in the link: a plain track (no made-up data).
            p.drawRoundedRect(QRectF(wave.left(), kCenterY - 1.5, wave.width(), 3.0), 1.5, 1.5);
            return;
        }
        const int             count  = voiceBarCount(wave.width());
        const QVector<quint8> levels = waveform::resample(e.link.waveform, count);
        for (int i = 0; i < count; ++i) {
            const qreal h = kBarMin + kBarRange * levels.at(i) / 15.0;
            const QRectF bar(wave.left() + i * (kBar + kBarGap), kCenterY - h / 2.0, kBar, h);
            p.drawRoundedRect(bar, kBar / 2.0, kBar / 2.0);
        }
    };
    paint(colors.unplayed);
    if (fraction > 0.0) {
        p.save();
        p.setClipRect(QRectF(wave.left() - 1.0, 0.0, split - wave.left() + 1.0, g.card.height()));
        paint(colors.played);
        p.restore();
    }
    if (isStarted(o)) {
        // The playhead: progress is not shown by colour alone.
        p.setBrush(colors.played);
        p.drawRoundedRect(QRectF(qBound(wave.left(), split - 1.0, wave.right() - 2.0), kCenterY - 13.0, 2.0, 26.0), 1.0, 1.0);
    }
}

} // namespace

bool isVoiceCard(const MediaEntry& entry)
{
    return entry.kind == MediaKind::Audio && entry.link.voice;
}

QSize voiceCardSize(const PreviewStyle& style)
{
    return QSize(qBound(kMinWidth, style.maxWidth, kMaxWidth), kHeight);
}

QRectF voiceWaveRect(const QSize& logicalSize)
{
    return geometryFor(QSizeF(logicalSize)).wave;
}

int voiceBarCount(qreal waveWidth)
{
    return qMax(0, static_cast<int>(std::floor((waveWidth + kBarGap) / (kBar + kBarGap))));
}

QImage renderVoiceCard(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize)
{
    const pp::Palette pal  = pp::palette(style.dark);
    const QSize       size = voiceCardSize(style);
    if (logicalSize)
        *logicalSize = size;
    const qreal w = size.width();
    const qreal h = size.height();

    QImage   out = pp::canvas(size, pp::ratio(style));
    QPainter p(&out);
    pp::prepare(p);
    p.setLayoutDirection(Qt::LeftToRight);

    const VoiceGeometry g          = geometryFor(QSizeF(size));
    const bool          actionable = isPreviewActionable(entry) || overlay.externalOnly;
    const bool          hovered    = style.hovered && actionable;
    const QColor        background = hovered ? pal.backgroundHover : pal.background;
    p.setPen(QPen(pal.border, 1));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), pp::cornerRadius(), pp::cornerRadius());

    drawButton(p, g, entry, overlay, style, pal, background, actionable);

    const QFont         meta = timeFont(style);
    const QFontMetricsF fm(meta);
    p.setFont(meta);
    const QStringList status = statusTexts(entry, overlay);
    if (!status.isEmpty()) {
        // The message takes the waveform's and the time's room.
        const QRectF area(g.wave.left(), kCenterY - 10.0, w - kRightPad - g.wave.left(), 20.0);
        p.setPen(showsFailure(entry, overlay) ? pal.errorText : pal.muted);
        p.drawText(area, Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute, pp::fitText(fm, status, area.width()));
        return out;
    }
    drawWave(p, g, entry, overlay, waveColors(style.dark));
    if (!g.time.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(g.time, Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, timeText(entry, overlay));
    }
    return out;
}

VideoZone voiceZoneAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    Q_UNUSED(style);
    const QRectF bounds(QPointF(0, 0), QSizeF(logicalSize));
    if (logicalSize.isEmpty() || !bounds.contains(pos))
        return VideoZone::None;
    const VoiceGeometry g = geometryFor(bounds.size());
    if (g.discHit.contains(pos))
        return VideoZone::PlayPause;
    if (seekable(entry, overlay) && g.seekHit.contains(pos))
        return VideoZone::Seek;
    return VideoZone::Body;
}

QRectF voiceZoneRect(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    const QRectF        bounds(QPointF(0, 0), QSizeF(logicalSize));
    const VoiceGeometry g = geometryFor(bounds.size());
    switch (voiceZoneAt(entry, overlay, style, logicalSize, pos)) {
    case VideoZone::None:
        return {};
    case VideoZone::PlayPause:
        return g.discHit;
    case VideoZone::Seek:
        return g.seekHit;
    case VideoZone::Body:
    case VideoZone::Mute:
    case VideoZone::Expand:
        break;
    }
    return bounds;
}

double voiceSeekFractionAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    Q_UNUSED(entry);
    Q_UNUSED(overlay);
    Q_UNUSED(style);
    const VoiceGeometry g = geometryFor(QSizeF(logicalSize));
    if (g.wave.width() <= 0.0)
        return 0.0;
    return qBound(0.0, (pos.x() - g.wave.left()) / g.wave.width(), 1.0);
}

QString voiceZoneToolTip(const MediaEntry& entry, const PlaybackOverlay& overlay, VideoZone zone, double seekFraction)
{
    if (overlay.busy) {
        if (entry.state == MediaState::Downloading)
            return i18n::t("Downloading… Click to cancel autoplay.");
        if (entry.state == MediaState::Ready)
            return i18n::t("Opening… Click to cancel autoplay.");
        return i18n::t("Waiting to download… Click to cancel autoplay.");
    }
    switch (zone) {
    case VideoZone::PlayPause:
        if (overlay.externalOnly)
            return i18n::t("Open in default app");
        if (entry.state == MediaState::Failed)
            return isRetryableDownload(entry) ? i18n::t("Retry download") : QString();
        return overlay.playing ? i18n::t("Pause") : i18n::t("Play");
    case VideoZone::Seek: {
        const qint64 duration = durationOf(entry, overlay);
        if (duration <= 0)
            return {};
        return i18n::t("Jump to %1").arg(formatDuration(qRound64(qBound(0.0, seekFraction, 1.0) * static_cast<double>(duration))));
    }
    case VideoZone::None:
    case VideoZone::Body:
    case VideoZone::Mute:
    case VideoZone::Expand:
        break;
    }
    return {};
}

QString voiceToolTipDetail(const MediaEntry& entry, const PlaybackOverlay& overlay)
{
    if (overlay.externalOnly)
        return i18n::t("Windows can't play this file here, so it opens in your default app.");
    if (entry.state == MediaState::Failed) {
        QString detail = entry.errorText.isEmpty() ? downloadErrorText(entry.error) : entry.errorText;
        if (isRetryableDownload(entry))
            detail += QLatin1Char(' ') + i18n::t("Click to retry.");
        return detail;
    }
    if (entry.state == MediaState::Downloading || entry.state == MediaState::Queued)
        return previewStatusText(entry, false);
    return {};
}

QString voiceToolTipName(const MediaEntry& entry)
{
    const qint64 duration = qMax<qint64>(0, entry.link.durationMs);
    return duration > 0 ? i18n::t("Voice message · %1").arg(formatDuration(duration)) : i18n::t("Voice message");
}

QVector<PreviewColorPair> voiceCardColorPairs(bool dark)
{
    const pp::Palette         pal    = pp::palette(dark);
    const WaveColors          colors = waveColors(dark);
    QVector<PreviewColorPair> pairs;
    auto add = [&pairs](const char* name, const QColor& foreground, const QColor& background, double minimum) {
        pairs.append({QString::fromLatin1(name), ui::flatten(foreground, background), background, minimum});
    };
    for (const bool hover : {false, true}) {
        const QColor bg = hover ? pal.backgroundHover : pal.background;
        add(hover ? "voice played bars (hover)" : "voice played bars", colors.played, bg, 3.0);
        add(hover ? "voice unplayed bars (hover)" : "voice unplayed bars", colors.unplayed, bg, 3.0);
        add(hover ? "voice time (hover)" : "voice time", pal.muted, bg, 4.5);
        add(hover ? "voice error line (hover)" : "voice error line", pal.errorText, bg, 4.5);
    }
    // Played against unplayed: told apart by brightness as well as the playhead (1.5:1 or more).
    add("voice played vs unplayed bars", colors.played, colors.unplayed, 1.5);
    add("voice icon on button", Qt::white, pp::accent(), 4.5);
    return pairs;
}
