#pragma once

// 2.2 voice: the voice message card in the chat: 320 x 56 (narrower chats: down to 160), a round play
// button, the waveform from the link (wf, 64 levels resampled to as many 3 px bars as fit) and the time
// ("0:12" at rest, the elapsed time while playing or paused). Played bars take the played colour and a
// 2 px playhead marks the position, so progress isn't shown by colour alone. A link without wf gets a
// plain track instead of made-up bars.
//
// Voice messages are audio entries (MediaKind::Audio with link.voice): audiocard.cpp hands them to the
// functions here, so the chat's audio card code (InlineMediaController Mode::Audio, hit tests, tooltips)
// covers them unchanged. Pure drawing and hit testing with one shared geometry function, like
// audiocard.h; logical pixels, images at style.dpr.
//
// Zones: PlayPause = the button (plus 4 px), Seek = the waveform (at its full height; before the file
// plays, a click starts it there), Body = the rest (toggles playback).

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include "previewrenderer.h"

// A voice message (Audio entry whose link carries vm=1): drawn as this card.
bool isVoiceCard(const MediaEntry& entry);

// Logical size: min(320, max(160, style.maxWidth)) x 56; previewLogicalSize() returns it for voice cards.
QSize voiceCardSize(const PreviewStyle& style);

QImage    renderVoiceCard(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize);
VideoZone voiceZoneAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);
QRectF    voiceZoneRect(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);
double    voiceSeekFractionAt(const MediaEntry& entry, const PlaybackOverlay& overlay, const PreviewStyle& style, const QSize& logicalSize, const QPointF& pos);

// Tooltips (i18n::t / arg() only: QToolTip's label outlives the plugin), like audiocard.h.
QString voiceZoneToolTip(const MediaEntry& entry, const PlaybackOverlay& overlay, VideoZone zone, double seekFraction);
QString voiceToolTipDetail(const MediaEntry& entry, const PlaybackOverlay& overlay);
// The first line of the card's tooltip, instead of the file name: "Voice message · 0:12".
QString voiceToolTipName(const MediaEntry& entry);

// The waveform rectangle of a card of that size (tests, the gallery's no-jump check).
QRectF voiceWaveRect(const QSize& logicalSize);
// Bars that fit into a waveform of that width (3 px bars, 2 px gaps).
int voiceBarCount(qreal waveWidth);

QVector<PreviewColorPair> voiceCardColorPairs(bool dark);
