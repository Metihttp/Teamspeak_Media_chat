#pragma once

// 2.2 spoiler: what the viewer shows over a hidden spoiler (MediaViewer puts it over its stage): the
// blurred picture filling the stage, an eye-off mark, a "Reveal spoiler" button and the hint "Click or
// press Space to reveal". A click anywhere on it or on the button asks for the reveal (the viewer maps
// Space and Enter to it too). While shown it takes every click and wheel turn, so nothing under it can
// be dragged, zoomed or played. After the reveal it fades out (instant when Windows animations are
// off) and keeps taking clicks for a double click's time, so the second click of a double click
// doesn't also play or zoom what was just revealed.

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

class QPushButton;
class QTimer;

class SpoilerCover : public QWidget
{
    Q_OBJECT

  public:
    explicit SpoilerCover(QWidget* parent = nullptr);

    // Shows the cover at once (a fade-out stops): picture and isBlurHash as spoiler::coverSource gives them.
    void cover(const QImage& picture, bool isBlurHash);
    void setPicture(const QImage& picture, bool isBlurHash); // a better picture arrived
    void fadeOut();                                          // revealed: fades out, then hides
    void dismiss();                                          // gone at once (another item is shown)
    bool isCovering() const;                                 // shown and not fading out

    QPushButton* button() const { return m_button; }

  signals:
    void revealRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    void    paintCover(QPainter& p);
    void    placeContent();
    qreal   opacity() const;
    QString hintText() const;

    QImage        m_picture;
    bool          m_blurHash  = false;
    QPushButton*  m_button    = nullptr;
    QTimer*       m_fadeTimer = nullptr;
    QElapsedTimer m_fadeClock; // valid while fading out
    bool          m_pressed = false;
    QRectF        m_eye;  // the eye-off mark above the button
    QRectF        m_hint; // the hint below it
};
