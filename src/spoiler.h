#pragma once

// 2.2 spoiler: pictures, GIFs and videos sent with sp=1 stay under a blurred cover until the receiver
// reveals them. This file holds the parts without widgets, so the unit tests and tools/render_gallery
// use them too (QtCore/QtGui only):
//  * SpoilerState: which chat media are spoilers and which the user revealed (Core keeps one for the
//    TeamSpeak session);
//  * the cover drawing shared by the chat previews, the viewer and the send window's thumbnails
//    (blurredStill, drawCover, drawEyeOffIcon);
//  * RevealFades: the chat's short crossfade from the cover to the content.

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>

#include "medialink.h"

class QPainter;
class QTimer;

namespace spoiler {

// sp=1 only counts for these (MediaLink::dropInvalidMetadata already drops it on other files).
bool appliesTo(MediaKind kind);

// What a hidden spoiler is called where its name would give it away (viewer title, a card): "Spoiler
// (image)", "Spoiler (GIF)" or "Spoiler (video)", worded like the chat link's label (linkLabel).
QString label(MediaKind kind);

// The cover is made from a copy of the picture whose longer side is this many pixels, so no detail
// smaller than about a twelfth of the preview survives (faces, text and shapes are gone).
constexpr int kCoverDetail = 12;
// Black over the blurred picture: a BlurHash is already soft and gets a little less.
constexpr int kDimAlpha         = 110;
constexpr int kBlurHashDimAlpha = 90;
// The "SPOILER" pill: black at these alphas behind white text (at least 6.9:1 over white pixels).
constexpr int kPillAlpha        = 165;
constexpr int kPillHoverAlpha   = 200;
constexpr int kPillPressedAlpha = 225;
// Reveal crossfade in the chat and the viewer (instant when Windows animations are off).
constexpr int kRevealMs = 150;
// The viewer's cover is darker than the chat's (its button and hint sit right on it): black at this
// alpha on top of drawCover's, so white text keeps 4.5:1 even over a white picture.
constexpr int kViewerDimAlpha         = 70;
constexpr int kViewerBlurHashDimAlpha = 85;
constexpr int kViewerHintAlpha        = 235; // the white hint under the viewer's button

// A soft copy of still to build a cover from: a BlurHash decode is soft already and is used as it is;
// anything sharper is shrunk until its longer side is kCoverDetail pixels and smoothly enlarged again
// (4x; drawCover stretches it the rest of the way). Null if still is null.
QImage blurredStill(const QImage& still, bool isBlurHash);

// What to cover a box with: the link's BlurHash decoded at the box's shape when it has a valid one (the
// cover then stays the same while the preview and the file arrive), otherwise still. *isBlurHash says
// which (pass both on to drawCover).
QImage coverSource(const QString& blurHash, const QSizeF& box, const QImage& still, bool stillIsBlurHash, bool* isBlurHash);

struct CoverLook {
    QFont  font;              // the pill's font (bold, the preview's text size minus 1 px)
    QColor placeholder;       // fill when there is no picture at all
    qreal  opacity  = 1.0;    // < 1 while the reveal crossfade runs; 0 draws nothing
    bool   hovered  = false;  // the pill darkens under the pointer...
    bool   pressed  = false;  // ...and a little more while the button is held
    bool   withPill = true;   // false: the blurred picture only (e.g. under a viewer's own button)
};

// Covers bounds (logical pixels; the caller clips to its rounded shape): the blurred still filling it
// (cover-cropped) and darkened, and a centred "SPOILER" pill, or a 24 px eye-off disc when the pill
// doesn't fit. A sharp picture is never drawn: still goes through blurredStill() first.
void drawCover(QPainter& p, const QRectF& bounds, const QImage& still, bool stillIsBlurHash, const CoverLook& look);

// Where drawCover puts the pill; empty when it doesn't fit and the eye-off disc is drawn instead.
QRectF pillRect(const QRectF& bounds, const QFont& font);
// The eye-off disc's place; empty when not even that fits (then the cover has no mark).
QRectF eyeOffDiscRect(const QRectF& bounds, const QFont& font);

// An eye with a slash through it (hidden content), stroked like the other preview icons.
void drawEyeOffIcon(QPainter& p, const QRectF& box, const QColor& color);

} // namespace spoiler

// Which chat media are spoilers and which of them the user revealed, for one TeamSpeak session (Core
// owns it; keys are MediaLink::key()). A key becomes a spoiler when any of its links carries sp=1 and
// stays one (key() ignores sp, so a repost without it doesn't uncover it). A key whose content was
// already drawn uncovered counts as revealed when an sp=1 link of it turns up later: a crafted repost
// can't cover what the user has already seen.
class SpoilerState
{
  public:
    // A link of key was seen in a chat. True when that made key a spoiler (its previews must change).
    bool noteSighting(const QString& key, bool spoiler);
    // The content of key was drawn without a cover (chat or viewer).
    void noteShownOpen(const QString& key);
    // Reveals (true) or covers again (false). True when that changed anything.
    bool setRevealed(const QString& key, bool revealed);

    bool isSpoiler(const QString& key) const { return m_spoilers.contains(key); }
    bool isRevealed(const QString& key) const { return m_revealed.contains(key); }
    // Covered right now: a spoiler that isn't revealed, unless every spoiler is shown (the setting).
    bool        isHidden(const QString& key, bool showAll) const;
    QStringList spoilers() const;
    void        clear();

  private:
    QSet<QString> m_spoilers;
    QSet<QString> m_revealed;
    QSet<QString> m_shownOpen;
};

// The chat's reveal crossfade: after start(key) the cover of key fades out over spoiler::kRevealMs
// (ease-out), and changed(key) asks for a redraw on every step and once more at the end. A child timer
// runs only while a fade does or a reveal is remembered; it is destroyed with its owner (never a
// pending functor timer).
class RevealFades : public QObject
{
    Q_OBJECT

  public:
    explicit RevealFades(QObject* parent = nullptr);

    // key was just revealed: its cover fades out, or (fade false: Windows animations are off) is gone
    // at once. Either way revealedWithin() knows about it.
    void  start(const QString& key, bool fade = true);
    void  cancel(const QString& key);             // the cover is back (Hide spoiler): no fade
    qreal coverOpacity(const QString& key) const; // 1 at the start .. 0 at the end; 0 when not fading
    bool  isFading(const QString& key) const;
    // key was revealed less than ms ago (the second click of a double click must not play or open it).
    bool revealedWithin(const QString& key, qint64 ms) const;

  signals:
    void changed(const QString& key);

  private:
    struct Fade {
        qint64 started  = 0;     // m_clock time of the reveal
        bool   fading   = true;  // false: revealed without a fade
        bool   finished = false; // drawn uncovered after its end (kept a while for revealedWithin)
    };
    void tick();

    QTimer*              m_timer = nullptr;
    QElapsedTimer        m_clock;
    QHash<QString, Fade> m_fades;
};
