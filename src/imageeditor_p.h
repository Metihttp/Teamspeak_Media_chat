#pragma once

// 2.2 editor: the parts of ImageEditor (imageeditor.h) - its colours, glyphs, buttons and the canvas.
// Private to imageeditor.cpp and imageeditorcanvas.cpp.

#include <QAbstractButton>
#include <QImage>
#include <QPointer>
#include <QWidget>

#include "imageeditor.h"

class QLineEdit;

namespace editorui {

// The window's colours, from TeamSpeak's palette (light or dark skin) and the 2.1 tokens.
struct Colors {
    bool   dark = false;
    QColor window;
    QColor text;
    QColor muted;  // secondary text, at least 4.5:1
    QColor accent; // checked tools, focus rings, the text box (3:1 or more)
    QColor error;  // at least 4.5:1
    QColor frame;  // separators, swatch rings
    QColor canvas; // behind the picture: #1e1f22 dark, #ebedef light
    QColor checkerA;
    QColor checkerB;
};

Colors colorsFor(const QPalette& palette);
QColor withAlpha(QColor color, qreal alpha);

enum class Glyph { None, Crop, Pen, Arrow, Rect, Text, Hide, Undo, Redo, Rotate, Swap };

// Vector glyphs in the stroke style of the plugin's other glyphs (box: usually 18 x 18 logical px).
void drawGlyph(QPainter& painter, Glyph glyph, const QRectF& box, const QColor& color);
// A filled disc with "!" (never colour alone).
void drawAlert(QPainter& painter, const QRectF& box, const QColor& fill, const QColor& mark);

} // namespace editorui

// A button of the toolbar or the options bar: a glyph (tools, undo/redo), text (aspect presets, modes),
// a colour swatch, or a dot (line widths). Checkable ones show their state with an accent fill and
// border; tab focus only (a click leaves the focus on the canvas), with a ring after keyboard focus.
class ImageEditor::Button : public QAbstractButton
{
  public:
    enum class Kind { Glyph, Text, Swatch, Dot };

    Button(Kind kind, QWidget* parent);

    void setGlyph(editorui::Glyph glyph);
    void setSwatch(const QColor& color);
    void setDot(qreal diameter);
    void setSide(int side); // square buttons
    void setColors(const editorui::Colors& colors);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    Kind             m_kind;
    editorui::Glyph  m_glyph = editorui::Glyph::None;
    QColor           m_swatch;
    qreal            m_dot  = 6;
    int              m_side = 32;
    editorui::Colors m_colors;
    bool             m_keyboardFocus = false;
};

// The picture: shows the edited picture (or, with the crop tool, the whole picture with the crop on
// it), takes the drawing, zooms (Ctrl+wheel, Ctrl+0, Ctrl+1) and pans (Space+drag, middle button,
// wheel). Everything is drawn from the editor's model through one transform, at the screen's device
// pixel ratio.
class ImageEditor::Canvas : public QWidget
{
  public:
    explicit Canvas(ImageEditor* editor);
    ~Canvas() override;

    void  setColors(const editorui::Colors& colors);
    void  setMessage(const QString& text, bool error); // instead of the picture (opening, can't open)
    void  resetView();  // a new picture
    void  invalidate(); // the model or the view changed
    void  fit();
    void  actualSize();
    qreal zoomFactor() const; // device pixels per picture pixel
    qreal scale() const;      // logical pixels per picture pixel

    void  zoomAt(qreal factor, const QPointF& anchor);

    bool cancelInteraction(); // Esc: true if a drag or the text box was cancelled
    void commitText();
    bool hasText() const;     // a text box with text in it
    bool textOpen() const { return !m_text.isNull(); }
    void toolChanged();
    void styleChanged();      // colour or size while the text box is open

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    enum class Drag { None, Pan, Draw, Crop, NewCrop };

    QRect                region() const; // rotated-picture pixels on show (all of it with the crop tool)
    qreal                fitScale() const;
    QTransform           rotatedToWidget() const;
    QTransform           baseToWidget() const;
    QTransform           baseToDevice() const;
    QPointF              widgetToBase(const QPointF& pos) const;
    QPointF              widgetToRotated(const QPointF& pos) const;
    QRectF               cropOnScreen() const;
    imageedit::Handle    handleAt(const QPointF& pos) const;
    void                 updateCursor(const QPointF& pos);
    void                 clampCenter();
    void                 leaveFit();
    void                 viewChanged();
    const QImage*        sourceFor(qreal deviceScale);
    void                 rebuildComposite();
    void                 drawLive(QPainter& painter);
    void                 updateLive(const QPointF& pos, bool shift);
    void                 finishDraw();
    void                 beginText(const QPointF& pos);
    void                 layoutText();
    void                 closeText();

    ImageEditor*      m_editor;
    editorui::Colors  m_colors;
    QString           m_message;
    bool              m_messageError = false;

    bool              m_fit  = true;
    qreal             m_zoom = 1.0; // device pixels per picture pixel, when not fitted
    QPointF           m_center;     // rotated-picture point in the middle, when not fitted
    QVector<QImage>   m_mips;       // the picture at 1/2, 1/4, ... (index 0 unused)
    QImage            m_composite;  // the model drawn at the current view, device pixels
    bool              m_dirty = true;

    Drag              m_drag = Drag::None;
    QPointF           m_pressPos;
    QPointF           m_pressCenter;
    imageedit::Handle m_handle = imageedit::Handle::None;
    QRect             m_cropStart;     // rotated pixels, when the crop drag started
    QRect             m_cropStartBase; // the pending crop then (Esc restores it)
    bool              m_space = false;
    bool              m_hasLive = false;
    imageedit::Shape  m_live;
    QPointF           m_liveStart;     // base pixels

    QPointer<QLineEdit> m_text;
    QPointF             m_textAnchor;  // base pixels: the text's top left corner
    qreal               m_textFontPx = 0;
    int                 m_textRotation = 0;
};
