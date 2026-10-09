#include "audiocard.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

#include "i18n.h"
#include "previewpaint.h"
#include "uiutil.h"
#include "voicecard.h" // 2.2 voice: voice messages are audio entries drawn as the voice card

namespace pp = previewpaint;

namespace {

// Layout (logical px) of the 64 px card: button at the left, two rows of text to its right.
constexpr qreal kDiscX       = 12.0;
constexpr qreal kDisc        = 40.0;
constexpr qreal kDiscHitPad  = 4.0;
constexpr qreal kTextLeft    = 64.0;
constexpr qreal kRightPad    = 12.0;
constexpr qreal kGap         = 8.0;
constexpr qreal kRowHeight   = 20.0;
constexpr qreal kRow1Center  = 20.0;
constexpr qreal kRow2Center  = 44.0;
constexpr qreal kSeekBand    = 20.0; // hit band around the seek bar
constexpr qreal kTrack       = 3.0;
constexpr qreal kTrackHover  = 5.0;
constexpr qreal kKnob        = 6.0;
constexpr qreal kMinTrack    = 40.0; // shorter: the time drops its total, then the time goes
constexpr qreal kMinTitle    = 64.0; // narrower: the size gives way to the name
constexpr qreal kBadge       = 16.0;

// The accent button's hover and pressed fills (white icons stay above 4.5:1 on all three).
const QColor kAccentHover(0x47, 0x52, 0xc4);
const QColor kAccentPressed(0x3c, 0x45, 0xa5);

struct AudioGeometry {
    QRectF card;
    QRectF disc;    // the round play button
    QRectF discHit; // its click target
    QRectF row2;    // second text row (seek bar and time, or a status line)
    QRectF track;   // the seek bar at rest (3 px)
    QRectF seekHit;
    QRectF time;    // right-aligned time column; empty when hidden
    bool   timeWithTotal = true;
};

QFont titleFont(const PreviewStyle& style)
{
    return pp::font(style, 0, true);
}

QFont metaFont(const PreviewStyle& style)
{
    return pp::font(style, -1.5);
}

qint64 durationOf(const MediaEntry& e, const PlaybackOverlay& o)
{
    return o.durationMs > 0 ? o.durationMs : qMax<qint64>(0, e.link.durationMs);
}

// The widest time text the column has to hold for this duration ("0:00 / 0:00", "00:00 / 00:00",
// "0:00:00 / 0:00:00"), so the column never changes width while the time counts up.
QString timePattern(qint64 durationMs, bool withTotal)
{
    const QString one = durationMs >= 3600 * 1000 ? QString::fromLatin1("0:00:00") : durationMs >= 600 * 1000 ? QString::fromLatin1("00:00") : QString::fromLatin1("0:00");
    return withTotal ? one + QString::fromLatin1(" / ") + one : one;
}

// Shared by renderAudioCard() and the hit tests.
AudioGeometry audioGeometry(const QSizeF& size, const PreviewStyle& style, qint64 durationMs)
{
    AudioGeometry g;
    const qreal   w     = size.width();
    const qreal   h     = size.height();
    const qreal   right = w - kRightPad;
    g.card              = QRectF(QPointF(0, 0), size);
    g.disc              = QRectF(kDiscX, (h - kDisc) / 2.0, kDisc, kDisc);
    g.discHit           = g.disc.adjusted(-kDiscHitPad, -kDiscHitPad, kDiscHitPad, kDiscHitPad);
    g.row2              = QRectF(kTextLeft, kRow2Center - kRowHeight / 2.0, qMax(0.0, right - kTextLeft), kRowHeight);

    const QFontMetricsF fm(metaFont(style));
    qreal               timeW  = qCeil(fm.horizontalAdvance(timePattern(durationMs, true)));
    qreal               trackW = right - kGap - timeW - kTextLeft;
    if (trackW < kMinTrack) {
        g.timeWithTotal = false;
        timeW           = qCeil(fm.horizontalAdvance(timePattern(durationMs, false)));
        trackW          = right - kGap - timeW - kTextLeft;
    }
    if (trackW < kMinTrack / 2.0) {
        timeW  = 0.0;
        trackW = right - kTextLeft;
    }
    if (timeW > 0.0)
        g.time = QRectF(right - timeW, g.row2.top(), timeW, kRowHeight);
    g.track   = QRectF(kTextLeft, kRow2Center - kTrack / 2.0, qMax(0.0, trackW), kTrack);
    g.seekHit = QRectF(kTextLeft - 6.0, kRow2Center - kSeekBand / 2.0, g.track.width() + 12.0, kSeekBand);
    return g;
}

bool isStarted(const PlaybackOverlay& o)
{
    return o.playing || o.ended || o.positionMs > 0;
}

// A failed download (not a player that could not open the file: that is externalOnly).
bool showsFailure(const MediaEntry& e, const PlaybackOverlay& o)
{
    return e.state == MediaState::Failed && !o.busy && !o.externalOnly;
}

// What the second row says instead of the seek bar, from most to least complete; empty = the seek bar.
QStringList statusTexts(const MediaEntry& e, const PlaybackOverlay& o)
{
    if (o.externalOnly)
        return {i18n::t("Can't play here · Click to open in your default app"), i18n::t("Can't play here · Click to open"), i18n::t("Click to open")};
    if (showsFailure(e, o)) {
        const QString title = downloadErrorTitle(e.error);
        if (isRetryableDownload(e))
            return {i18n::t("%1 · Click to retry").arg(title), title};
        return {title};
    }
    if (e.state == MediaState::Downloading) {
        const int   percent = qRound(qBound(0.0, e.progress, 1.0) * 100.0);
        QStringList texts;
        if (e.link.size) {
            const auto done = static_cast<quint64>(qBound(0.0, e.progress, 1.0) * static_cast<double>(e.link.size) + 0.5);
            texts << i18n::t("Downloading… %1% · %2").arg(percent).arg(formatProgress(done, e.link.size));
        }
        texts << i18n::t("Downloading… %1%").arg(percent) << i18n::t("%1%").arg(percent);
        return texts;
    }
    if (e.state == MediaState::Queued)
        return {i18n::t("Waiting to download…"), i18n::t("Waiting…")};
    if (durationOf(e, o) <= 0 && !isStarted(o))
        return {i18n::t("Click to play")}; // plain TeamSpeak links carry no duration
    return {};
}

bool seekShown(const MediaEntry& e, const PlaybackOverlay& o)
{
    return statusTexts(e, o).isEmpty();
}

bool seekable(const MediaEntry& e, const PlaybackOverlay& o)
{
    return seekShown(e, o) && durationOf(e, o) > 0;
}

double playedFraction(const PlaybackOverlay& o, qint64 durationMs)
{
    if (o.ended)
        return 1.0;
    if (durationMs <= 0)
        return 0.0;
    return qBound(0.0, static_cast<double>(o.positionMs) / static_cast<double>(durationMs), 1.0);
}

// A small round badge on the lower right of the button, cut out of the card with its background.
QRectF badgeRect(const QRectF& disc)
{
    return QRectF(disc.right() - kBadge + 3.0, disc.bottom() - kBadge + 3.0, kBadge, kBadge);
}

void drawBadgeCutout(QPainter& p, const QRectF& badge, const QColor& cardBackground)
{
    p.setPen(Qt::NoPen);
    p.setBrush(cardBackground);
    p.drawEllipse(badge.adjusted(-2.0, -2.0, 2.0, 2.0));
}

void drawButton(QPainter& p, const AudioGeometry& g, const MediaEntry& e, const PlaybackOverlay& o, const PreviewStyle& style, const pp::Palette& pal,
                const QColor& cardBackground, bool actionable)
{
    const QRectF disc    = g.disc;
    const bool   failed  = showsFailure(e, o);
    const bool   hot     = actionable && (o.hover == VideoZone::PlayPause || o.hover == VideoZone::Body);
    const bool   down    = actionable && (o.pressed == VideoZone::PlayPause || o.pressed == VideoZone::Body);
    const QRectF icon    = disc.adjusted(11.0, 11.0, -11.0, -11.0);
    const QRectF arrowIn = icon.adjusted(-1.5, -1.5, 1.5, 1.5);

    p.save();
    if (down) { // pressed: the button shrinks a little (feedback before the release acts)
        p.translate(disc.center());
        p.scale(0.96, 0.96);
        p.translate(-disc.center());
    }
    if (failed && !isRetryableDownload(e)) {
        // Gone from the server / password-protected: nothing to press, just the alert.
        pp::drawAlertIcon(p, disc, pal.errorFill, Qt::white);
        p.restore();
        return;
    }
    // On the dark card the accent alone is just under 3:1: a 1 px edge in the tonal accent keeps the
    // button's outline visible (4.2:1) while the white icon keeps 4.6:1 on the fill.
    if (style.dark)
        p.setPen(QPen(pal.progress, 1.0));
    else
        p.setPen(Qt::NoPen);
    p.setBrush(down ? kAccentPressed : hot ? kAccentHover : pp::accent());
    p.drawEllipse(style.dark ? disc.adjusted(0.5, 0.5, -0.5, -0.5) : disc);

    if (o.busy) {
        const qreal stroke = 2.5;
        pp::drawRing(p, disc.center(), disc.width() / 2.0 - stroke - 5.0, stroke, o.busyProgress, QColor(255, 255, 255, 70), Qt::white, style.animate);
    } else if (failed) {
        pp::drawCircularArrow(p, arrowIn, Qt::white); // click to retry
    } else if (o.externalOnly) {
        pp::drawOpenIcon(p, icon, Qt::white);
    } else if (o.playing) {
        pp::drawPauseIcon(p, icon, Qt::white);
    } else if (o.ended) {
        pp::drawCircularArrow(p, arrowIn, Qt::white);
    } else {
        pp::drawPlayIcon(p, icon.translated(1.0, 0.0), Qt::white); // nudged right: the triangle's optical centre
    }
    p.restore();

    // Badges are not part of the press: they say something about the file, not the button.
    if (failed) {
        const QRectF badge = badgeRect(disc);
        drawBadgeCutout(p, badge, cardBackground);
        pp::drawAlertIcon(p, badge, pal.errorFill, Qt::white);
    } else if (e.state == MediaState::Idle && !o.busy && !o.externalOnly && !isStarted(o)) {
        // Not downloaded yet: pressing play fetches it first.
        const QRectF badge = badgeRect(disc);
        drawBadgeCutout(p, badge, cardBackground);
        p.setBrush(pal.title);
        p.drawEllipse(badge);
        pp::drawDownloadIcon(p, badge.adjusted(3.5, 3.5, -3.5, -3.5), cardBackground);
    }
}

void drawSeekBar(QPainter& p, const AudioGeometry& g, const PlaybackOverlay& o, qint64 durationMs, const pp::Palette& pal, const QColor& cardBackground)
{
    const bool   seeking   = o.hover == VideoZone::Seek || o.pressed == VideoZone::Seek;
    const qreal  thickness = seeking ? kTrackHover : kTrack;
    const QRectF track(g.track.left(), kRow2Center - thickness / 2.0, g.track.width(), thickness);
    if (track.width() <= 0.0)
        return;
    p.setPen(Qt::NoPen);
    p.setBrush(pal.track);
    p.drawRoundedRect(track, thickness / 2.0, thickness / 2.0);
    const double fraction = playedFraction(o, durationMs);
    if (fraction > 0.0) {
        p.setBrush(pal.progress);
        p.drawRoundedRect(QRectF(track.topLeft(), QSizeF(qMax(thickness, track.width() * fraction), thickness)), thickness / 2.0, thickness / 2.0);
    }
    if (seeking) {
        const QPointF knob(track.left() + track.width() * fraction, track.center().y());
        p.setBrush(cardBackground);
        p.drawEllipse(knob, kKnob + 1.5, kKnob + 1.5);
        p.setBrush(pal.progress);
        p.drawEllipse(knob, kKnob, kKnob);
    }
}

QString timeText(const AudioGeometry& g, const PlaybackOverlay& o, qint64 durationMs)
{
    if (!isStarted(o))
        return formatDuration(durationMs);
    const qint64 position = o.ended ? durationMs : qMax<qint64>(0, o.positionMs);
    if (!g.timeWithTotal || durationMs <= 0)
        return formatDuration(position);
    return i18n::t("%1 / %2").arg(formatDuration(position), formatDuration(durationMs));
}

} // namespace

QSize audioCardSize(const MediaEntry& entry, const PreviewStyle& style)
{
    return previewLogicalSize(entry, style);
}

QImage renderAudioCard(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return renderVoiceCard(entry, overlay, style, logicalSize);
    const pp::Palette pal  = pp::palette(style.dark);
    const QSize       size = audioCardSize(entry, style);
    if (logicalSize)
        *logicalSize = size;
    const qreal w = size.width();
    const qreal h = size.height();

    QImage   out = pp::canvas(size, pp::ratio(style));
    QPainter p(&out);
    pp::prepare(p);
    p.setLayoutDirection(Qt::LeftToRight);

    const qint64        duration   = durationOf(entry, overlay);
    const AudioGeometry g          = audioGeometry(QSizeF(size), style, duration);
    const bool          actionable = isPreviewActionable(entry) || overlay.externalOnly;
    const bool          hovered    = style.hovered && actionable;
    const QColor        background = hovered ? pal.backgroundHover : pal.background;
    p.setPen(QPen(pal.border, 1));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), pp::cornerRadius(), pp::cornerRadius());

    drawButton(p, g, entry, overlay, style, pal, background, actionable);

    // Row 1: the name, and the size at the right while there is room for both.
    const QFont         title = titleFont(style);
    const QFont         meta  = metaFont(style);
    const QFontMetricsF titleFm(title);
    const QFontMetricsF metaFm(meta);
    const qreal         right    = w - kRightPad;
    const QString       sizeText = entry.link.size ? formatSize(entry.link.size) : QString();
    qreal               sizeW    = sizeText.isEmpty() ? 0.0 : qCeil(metaFm.horizontalAdvance(sizeText));
    qreal               nameEnd  = right - (sizeW > 0.0 ? sizeW + kGap : 0.0);
    if (nameEnd - kTextLeft < kMinTitle) {
        sizeW   = 0.0;
        nameEnd = right;
    }
    const QRectF nameRect(kTextLeft, kRow1Center - kRowHeight / 2.0, qMax(0.0, nameEnd - kTextLeft), kRowHeight);
    const auto   left = Qt::AlignVCenter | Qt::AlignLeft | Qt::AlignAbsolute;
    p.setFont(title);
    p.setPen(pal.title);
    pp::drawFileName(p, nameRect, left, titleFm.elidedText(displayNameFor(entry.link), Qt::ElideMiddle, nameRect.width()));
    if (sizeW > 0.0) {
        p.setFont(meta);
        p.setPen(pal.muted);
        p.drawText(QRectF(right - sizeW, nameRect.top(), sizeW, kRowHeight), Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, sizeText);
    }

    // Row 2: the seek bar and the time, or what is happening instead (downloading, an error).
    const QStringList status = statusTexts(entry, overlay);
    p.setFont(meta);
    if (!status.isEmpty()) {
        p.setPen(showsFailure(entry, overlay) ? pal.errorText : pal.muted);
        p.drawText(g.row2, left, pp::fitText(metaFm, status, g.row2.width()));
        return out;
    }
    drawSeekBar(p, g, overlay, duration, pal, background);
    if (!g.time.isEmpty() && duration > 0) {
        p.setFont(meta);
        p.setPen(pal.muted);
        p.drawText(g.time, Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, timeText(g, overlay, duration));
    }
    return out;
}

VideoZone audioZoneAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return voiceZoneAt(entry, overlay, style, logicalSize, pos);
    const QRectF bounds(QPointF(0, 0), QSizeF(logicalSize));
    if (logicalSize.isEmpty() || !bounds.contains(pos))
        return VideoZone::None;
    const AudioGeometry g = audioGeometry(bounds.size(), style, durationOf(entry, overlay));
    if (g.discHit.contains(pos))
        return VideoZone::PlayPause;
    if (seekable(entry, overlay) && g.seekHit.contains(pos))
        return VideoZone::Seek;
    return VideoZone::Body;
}

QRectF audioZoneRect(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return voiceZoneRect(entry, overlay, style, logicalSize, pos);
    const QRectF        bounds(QPointF(0, 0), QSizeF(logicalSize));
    const AudioGeometry g = audioGeometry(bounds.size(), style, durationOf(entry, overlay));
    switch (audioZoneAt(entry, overlay, style, logicalSize, pos)) {
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

double audioSeekFractionAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return voiceSeekFractionAt(entry, overlay, style, logicalSize, pos);
    const AudioGeometry g = audioGeometry(QSizeF(logicalSize), style, durationOf(entry, overlay));
    if (g.track.width() <= 0.0)
        return 0.0;
    return qBound(0.0, (pos.x() - g.track.left()) / g.track.width(), 1.0);
}

QString audioZoneToolTip(const MediaEntry& entry, const PlaybackOverlay& overlay, VideoZone zone, double seekFraction)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return voiceZoneToolTip(entry, overlay, zone, seekFraction);
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
        return overlay.playing ? i18n::t("Pause") : overlay.ended ? i18n::t("Replay") : i18n::t("Play");
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

QString audioToolTipDetail(const MediaEntry& entry, const PlaybackOverlay& overlay)
{
    if (isVoiceCard(entry)) // 2.2 voice
        return voiceToolTipDetail(entry, overlay);
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

QVector<PreviewColorPair> audioCardColorPairs(bool dark)
{
    const pp::Palette         pal = pp::palette(dark);
    QVector<PreviewColorPair> pairs;
    auto add = [&pairs](const char* name, const QColor& foreground, const QColor& background, double minimum) {
        pairs.append({QString::fromLatin1(name), ui::flatten(foreground, background), background, minimum});
    };
    for (const bool hover : {false, true}) {
        const QColor bg = hover ? pal.backgroundHover : pal.background;
        add(hover ? "audio name (hover)" : "audio name", pal.title, bg, 4.5);
        add(hover ? "audio time and size (hover)" : "audio time and size", pal.muted, bg, 4.5);
        add(hover ? "audio error line (hover)" : "audio error line", pal.errorText, bg, 4.5);
        add(hover ? "audio played bar vs card (hover)" : "audio played bar vs card", pal.progress, bg, 3.0);
        add(hover ? "audio download badge vs card (hover)" : "audio download badge vs card", pal.title, bg, 3.0);
        add(hover ? "audio download badge glyph (hover)" : "audio download badge glyph", bg, pal.title, 3.0);
        // The button's outline: its fill in the light theme, the tonal edge in the dark one.
        add(hover ? "audio button edge vs card (hover)" : "audio button edge vs card", dark ? pal.progress : pp::accent(), bg, 3.0);
    }
    add("audio played bar vs track", pal.progress, pal.track, 3.0);
    add("audio icon on button", Qt::white, pp::accent(), 4.5);
    add("audio icon on button (hover)", Qt::white, kAccentHover, 4.5);
    add("audio icon on button (pressed)", Qt::white, kAccentPressed, 4.5);
    add("audio busy ring on button", Qt::white, pp::accent(), 3.0);
    add("audio alert mark", Qt::white, pal.errorFill, 3.0);
    return pairs;
}
