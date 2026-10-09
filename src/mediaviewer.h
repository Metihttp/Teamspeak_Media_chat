#pragma once

// Full-size viewer for chat media (replaces the v1 ImageViewer): images with zoom/pan, animated
// GIF/WebP, and videos with a full player (play/pause, seek, time, volume/mute, loop, fullscreen).
// Shows a gallery: Left/Right (and buttons) move between the media of the chat it was opened from.
// Items that are not downloaded yet are fetched through Core with a progress display: pictures and
// the item the viewer was opened on right away; other videos / audio only within the automatic
// download limit (Settings::videoAutoDownloadMB), otherwise when the user presses play.
// Everything works from the keyboard: Tab reaches the buttons, ? (or F1) lists the shortcuts.
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.

#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QStringList>

#include <optional>

class Core;
class QThreadPool;
class QTimer;

// Zoomable image canvas: fit-to-window by default, wheel to zoom around the cursor, drag (or the
// arrow keys, through panBy) to pan, double-click toggles between fit and 100% (200% when the
// picture already fits at 100%). Zoom counts device pixels: 100% shows one image pixel per screen
// pixel, also on a scaled display. Strong downscales are drawn from a smoothly resampled copy that
// is made once the zoom stops changing (on a worker thread for very large pictures).
class ImageCanvas : public QWidget
{
    Q_OBJECT

  public:
    explicit ImageCanvas(QWidget* parent = nullptr);
    ~ImageCanvas() override; // waits for a resampling worker

    void  setImage(const QImage& image, bool resetView);
    void  setFit();
    void  setActualSize();
    void  zoomTo(qreal zoom, const QPointF& anchor); // anchor: the widget point that stays in place
    void  zoomBy(qreal factor);                      // around the centre
    void  panBy(const QPointF& delta);               // logical pixels
    void  setCursorHidden(bool hidden);              // full screen: no pointer over the picture
    void  screenScaleChanged();                      // the window moved to a monitor with another scale
    bool  canPan() const;
    bool  isFit() const;
    qreal zoom() const;    // device pixels per image pixel
    qreal fitZoom() const; // the zoom of setFit(); 1.0 when the picture fits at 100%

  signals:
    void zoomChanged(qreal zoom);
    void clicked();
    void activity(); // the pointer moved over the picture (at most every 50 ms)
    // 2.2 drag-out: a drag on a picture that can't be panned (it fits): drag the file out instead.
    void dragOutRequested(const QPoint& pressPos);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    qreal scale() const; // logical pixels per image pixel
    void  applyZoom();
    void  clampOffset();
    void  updateCursor();
    void  scheduleRescale();
    void  rescale();
    void  setDisplay(quint64 job, qreal zoom, qreal dpr, QImage image);

    QImage        m_image;
    bool          m_fit  = true;
    qreal         m_zoom = 1.0;
    QPointF       m_offset;
    QPoint        m_dragStart;
    QPointF       m_dragOffset;
    bool          m_dragging     = false;
    bool          m_moved        = false;
    bool          m_cursorHidden = false;
    QElapsedTimer m_activityClock;

    // m_image resampled for m_displayZoom at m_displayDpr (null while none matches).
    QImage       m_display;
    qreal        m_displayZoom = 0;
    qreal        m_displayDpr  = 0;
    quint64      m_displayJob  = 0; // results of older resampling jobs are dropped
    QTimer*      m_rescale     = nullptr;
    QThreadPool* m_pool        = nullptr;
};

class MediaViewer : public QDialog
{
    Q_OBJECT

  public:
    // keys: gallery in chat order; index: the item to show first.
    // Videos start muted according to Settings::videosStartMuted.
    static void open(Core* core, const QStringList& keys, int index);
    // Same, but videos start muted or not as given: pass the chat's session mute (the inline players'
    // state), so a clip muted in the chat does not play out loud once expanded. A viewer that is
    // already open takes it over. Returns the viewer (see mutedChanged), nullptr if nothing to show.
    static MediaViewer* open(Core* core, const QStringList& keys, int index, bool muted);

    // Pauses the video / audio being played, if any (e.g. because an inline video started).
    void pausePlayback();

  signals:
    // The user muted or unmuted playback in the viewer (lets the chat's inline players follow).
    void mutedChanged(bool muted);
    // Playback of a video / audio started here (autoplay, Play, Space, replay, ...): the chat's
    // inline players pause, so only one thing plays at a time.
    void playbackStarted();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    // ---- implementation (owned by mediaviewer.cpp; may be reorganised freely) ------------------
  private:
    // muted: std::nullopt = Settings::videosStartMuted (new viewer) / unchanged (open viewer).
    static MediaViewer* openViewer(Core* core, const QStringList& keys, int index, std::optional<bool> muted);
    MediaViewer(Core* core, const QStringList& keys, int index, std::optional<bool> muted, QWidget* parent);
    ~MediaViewer() override; // stops playback and releases the shown key; deleted via WA_DeleteOnClose / plugin shutdown
    struct Private;
    Private* d;
};
