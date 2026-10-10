#pragma once

// 2.2 emoji: one line of text with the table's emoji as HD pictures, for the plugin's own drawing (the
// reply line in the chat, the reply bar, the replies popup and the send window's "Replying to"). The
// text is laid out the way QPainter::drawText lays it out (one line, in its own direction), so widths,
// elision and right-to-left text stay what QFontMetricsF says; each emoji's glyph is left out and its
// picture (emojirender.h) drawn in the glyph's place. QtGui only; GUI thread.

#include <QString>

class QFontMetricsF;
class QPainter;

namespace emoji {

// Draws text from x (its left edge) on baseline with the painter's font and pen, as
// p.drawText(QRectF(x, baseline - fm.ascent(), fm.horizontalAdvance(text) + 2, fm.height()),
// Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine | Qt::TextForce{Left,Right}ToLeft, text) would. fm: the
// painter font's metrics. At most 24 emoji become pictures; the rest stay text.
void drawTextLine(QPainter& p, qreal x, qreal baseline, const QFontMetricsF& fm, const QString& text, bool rightToLeft);

// Whether drawTextLine draws pictures: on by default; ChatEmoji follows "Show emoji in high quality in
// the chat" (and turns it off when it goes, before emoji::shutdown()). Without the colour engine never.
void setTextPicturesEnabled(bool enabled);
bool textPicturesEnabled();

} // namespace emoji
