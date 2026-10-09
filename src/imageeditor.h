#pragma once

// 2.2 editor: the crop & annotate window the send window opens for a picture (Edit…). Tools: crop with
// aspect presets and rotate, pen, arrow, rectangle, text, and "Hide details" (pixelate or black box);
// six colours, three sizes, undo/redo, zoom and pan. What is drawn is kept by an ImageEditModel
// (imageeditmodel.h) in picture pixels, so the edited file is exact at full resolution whatever the
// zoom and the screen's scaling.
//
// The window only edits. Done emits doneRequested(): the send window then writes the edited copy on its
// worker (setSaving), and closes the editor or shows why it failed (setSaveError). Window-modal over the
// send window, deleted on close, objectName "tsmediaImageEditor" (plugin shutdown deletes it too).

#include <QDialog>
#include <QPointer>
#include <QVector>

#include "imageeditmodel.h"

class QAbstractButton;
class QLabel;
class QMessageBox;
class QPushButton;
class QStackedWidget;

class ImageEditor : public QDialog
{
    Q_OBJECT

  public:
    enum class Tool { Crop, Pen, Arrow, Rect, Text, Hide };
    enum class Aspect { Free, Original, Square, FourThree, SixteenNine };

    // name: the picture's name (the window title says it).
    ImageEditor(const QString& name, const imageedit::Prefs& prefs, QWidget* parent);
    ~ImageEditor() override;

    // Until one of these, the window says "Opening the picture…" and its tools are off.
    void setModel(const imageedit::ImageEditModel& model); // with its picture (base) set
    void setOpenError(const QString& text);
    bool isLoaded() const { return m_loaded; }

    // What was edited (after doneRequested: final, a pending crop applied).
    const imageedit::ImageEditModel& model() const { return m_model; }
    imageedit::Prefs                 prefs() const;

    void setSaving(bool saving); // "Saving…": everything is off until the owner answers
    void setSaveError(const QString& text);
    bool isSaving() const { return m_saving; }

    // For render tools and tests.
    void     setTool(Tool tool);
    Tool     tool() const { return m_tool; }
    void     setAspect(Aspect aspect);
    QWidget* canvasWidget() const;

  signals:
    void doneRequested();

  public slots:
    void reject() override; // Esc, Cancel, the close button: asks first when something changed here

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    class Canvas;
    class Button;

    void   finish();            // Done / Enter
    void   undo();
    void   redo();
    void   rotate();
    void   applyCrop();         // the pending crop becomes an undo step
    void   cancelCrop();
    void   resetCrop();
    void   swapAspect();
    double aspectValue() const; // width / height in the rotated picture; 0: free
    void   setColor(int index);
    void   setStroke(int index);
    void   setHideMode(int mode);
    void   nudgeCrop(int key, bool resize);
    void   modelChanged();      // after every change of m_model
    void   updateControls();
    void   updateStatus();
    void   applyTheme();
    bool   changedHere() const; // something to lose on Cancel
    QString hintText() const;

    imageedit::ImageEditModel m_model;
    imageedit::Doc            m_openedDoc; // as it was when the window got it
    QRect                     m_pendingCrop; // base pixels, while the crop tool is on
    Tool                      m_tool     = Tool::Pen;
    Tool                      m_drawTool = Tool::Pen; // where Esc / Enter go from the crop tool
    Aspect                    m_aspect   = Aspect::Free;
    bool                      m_portrait = false; // 4:3 and 16:9 turned to 3:4 and 9:16
    int                       m_color    = 0;
    int                       m_stroke   = 1;
    int                       m_hideMode = 0;
    bool                      m_loaded   = false;
    bool                      m_saving   = false;
    bool                      m_keyboardFocus = false;
    bool                      m_applyingTheme = false;
    QString                   m_name;

    Canvas*                   m_canvas = nullptr;
    QVector<Button*>          m_toolButtons; // in Tool order
    Button*                   m_undo   = nullptr;
    Button*                   m_redo   = nullptr;
    QPushButton*              m_cancel = nullptr;
    QPushButton*              m_done   = nullptr;
    QStackedWidget*           m_options = nullptr;
    QWidget*                  m_drawPage = nullptr;
    QWidget*                  m_cropPage = nullptr;
    QWidget*                  m_hidePage = nullptr;
    QVector<Button*>          m_swatches;
    QVector<Button*>          m_sizes;
    QVector<Button*>          m_aspects; // in Aspect order
    Button*                   m_swap      = nullptr;
    Button*                   m_rotate    = nullptr;
    Button*                   m_resetCrop = nullptr;
    QVector<Button*>          m_hideModes;
    QLabel*                   m_hideHint  = nullptr;
    QWidget*                  m_statusBar = nullptr;
    QLabel*                   m_sizeLabel = nullptr;
    QLabel*                   m_zoomLabel = nullptr;
    QLabel*                   m_hintLabel = nullptr;
    QLabel*                   m_errorIcon = nullptr;
    QLabel*                   m_errorText = nullptr;
    QPointer<QMessageBox>     m_discard;
};
