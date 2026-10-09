#pragma once

// Full-size viewer for chat media (replaces the v1 ImageViewer): images with zoom/pan, animated
// GIF/WebP, and videos with a full player (play/pause, seek, time, volume/mute, loop, fullscreen).
// Shows a gallery: Left/Right (and buttons) move between the media of the chat it was opened from.
// Items that are not downloaded yet are fetched through Core with a progress display: pictures and
// the item the viewer was opened on right away; other videos / audio only within the automatic
// download limit (Settings::videoAutoDownloadMB), otherwise when the user presses play.
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.

#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QStringList>

#include <optional>

class Core;

// Zoomable image canvas: fit-to-window by default, wheel to zoom around the cursor, drag to pan,
// double-click toggles between fit and 100%.
class ImageCanvas : public QWidget
{
    Q_OBJECT

  public:
    explicit ImageCanvas(QWidget* parent = nullptr);

    void  setImage(const QImage& image, bool resetView);
    void  setFit();
    void  setActualSize();
    qreal zoom() const;

  signals:
    void zoomChanged(qreal zoom);
    void clicked();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    qreal fitZoom() const;
    void  clampOffset();

    QImage  m_image;
    bool    m_fit  = true;
    qreal   m_zoom = 1.0;
    QPointF m_offset;
    QPoint  m_dragStart;
    QPointF m_dragOffset;
    bool    m_dragging = false;
    bool    m_moved    = false;
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
