#pragma once

// The inline audio player card (2.2): a 340 x 64 card for audio files (MediaKind::Audio) with a round
// play button, the file name and size, a seek bar and the time ("0:42 / 3:25"). The same box as the
// 2.1 file card (previewLogicalSize), so a chat never moves when a card turns into a player.
// Pure drawing and hit testing, like previewrenderer.h: the renderer and the hit tests share one
// geometry function, so what is drawn and what is clicked can never disagree. Sizes are logical
// pixels; the image has devicePixelRatio == style.dpr.
//
// Zones (VideoZone): PlayPause = the button (plus 4 px), Seek = the band around the seek bar (only
// while it is shown), Body = the rest of the card (toggles playback as well). No Mute / Expand: the
// volume is the one from Settings, and audio has nothing to expand into.

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include "previewrenderer.h"

// The card's logical size: always previewLogicalSize(entry, style).
QSize audioCardSize(const MediaEntry& entry, const PreviewStyle& style);

// overlay: InlineMediaController::overlay() (playing, busy, externalOnly, position, duration, hover,
// pressed); a default PlaybackOverlay with durationMs = link.durationMs draws the card at rest.
QImage renderAudioCard(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize);

// Hit testing for renderAudioCard() output of logicalSize; pos relative to its top-left. Pass the same
// entry, overlay and style the card was drawn with (the time column's width depends on the duration).
VideoZone audioZoneAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);
QRectF    audioZoneRect(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);
double    audioSeekFractionAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);

// The tooltip of a zone of the card ("Play", "Pause", "Replay", "Jump to 1:23", "Downloading… Click to
// cancel autoplay."), or empty where the caller shows the file's name and size (the body). Plain text
// from i18n::t / arg() only (QToolTip's label outlives the plugin).
QString audioZoneToolTip(const MediaEntry& entry, const PlaybackOverlay& overlay, VideoZone zone, double seekFraction);
// Extra line under the name in the body's tooltip (the full error text, "can't play here"), may be empty.
QString audioToolTipDetail(const MediaEntry& entry, const PlaybackOverlay& overlay);

// Text and icon colours of the card with what they are drawn on (tools/render_gallery checks them).
QVector<PreviewColorPair> audioCardColorPairs(bool dark);
