#include "imageeditor.h"

#include <QAccessible>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

#include "i18n.h"
#include "imageeditor_p.h"

using editorui::Colors;
using editorui::Glyph;

namespace {

constexpr int kToolbarHeight = 44;
constexpr int kOptionsHeight = 36;
constexpr int kStatusHeight  = 28;
constexpr int kMinWidth      = 720;
constexpr int kMinHeight     = 520;
constexpr int kOpenWidth     = 1280;
constexpr int kOpenHeight    = 860;

// Virtual keys: the letter and digit shortcuts also work on non-Latin keyboard layouts (as in the viewer).
constexpr quint32 kVk0 = 0x30;
constexpr quint32 kVk1 = 0x31;
constexpr quint32 kVkA = 0x41;
constexpr quint32 kVkC = 0x43;
constexpr quint32 kVkH = 0x48;
constexpr quint32 kVkP = 0x50;
constexpr quint32 kVkR = 0x52;
constexpr quint32 kVkT = 0x54;
constexpr quint32 kVkX = 0x58;
constexpr quint32 kVkY = 0x59;
constexpr quint32 kVkZ = 0x5A;
constexpr quint32 kVkOpenBracket  = 0xDB;
constexpr quint32 kVkCloseBracket = 0xDD;

// A 1 px line between the bars and the canvas.
class Separator : public QWidget
{
  public:
    Separator(Qt::Orientation orientation, QWidget* parent)
        : QWidget(parent)
    {
        if (orientation == Qt::Horizontal)
            setFixedHeight(1);
        else
            setFixedSize(1, 24);
    }
  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), editorui::colorsFor(palette()).frame);
    }
};

QLabel* roleLabel(const char* role, QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

void colorLabel(QLabel* label, const QColor& color)
{
    QPalette pal = label->palette();
    pal.setColor(QPalette::Active, QPalette::WindowText, color);
    pal.setColor(QPalette::Inactive, QPalette::WindowText, color);
    label->setPalette(pal);
}

} // namespace

ImageEditor::ImageEditor(const QString& name, const imageedit::Prefs& prefsIn, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint)
    , m_name(name)
{
    // fromLatin1: plugin shutdown finds it by this name; it lives among TeamSpeak's windows.
    setObjectName(QString::fromLatin1("tsmediaImageEditor"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setLayoutDirection(Qt::LeftToRight);
    setWindowTitle(i18n::t("Edit image — %1").arg(name));
    const imageedit::Prefs prefs = imageedit::clamped(prefsIn);
    m_color    = prefs.color;
    m_stroke   = prefs.stroke;
    m_hideMode = prefs.hideMode;

    setMinimumSize(kMinWidth, kMinHeight);
    const QScreen* screen = parent ? parent->screen() : this->screen();
    const QRect    avail  = screen ? screen->availableGeometry() : QRect(0, 0, 1600, 1000);
    resize(qMax(kMinWidth, qMin(kOpenWidth, avail.width() * 85 / 100)), qMax(kMinHeight, qMin(kOpenHeight, avail.height() * 85 / 100)));

    // ---- toolbar: tools, undo/redo, Cancel and Done ------------------------------------------------
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(kToolbarHeight);
    auto* tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(12, 6, 12, 6);
    tools->setSpacing(4);
    struct ToolInfo {
        Tool        tool;
        Glyph       glyph;
        const char* name;
        const char* tip;
    };
    const ToolInfo toolInfo[] = {
        {Tool::Crop, Glyph::Crop, "Crop", "Crop (C)"},
        {Tool::Pen, Glyph::Pen, "Pen", "Pen (P)"},
        {Tool::Arrow, Glyph::Arrow, "Arrow", "Arrow (A)"},
        {Tool::Rect, Glyph::Rect, "Rectangle", "Rectangle (R)"},
        {Tool::Text, Glyph::Text, "Text", "Text (T)"},
        {Tool::Hide, Glyph::Hide, "Hide details", "Hide details (H): pixelate or black out names, codes and addresses"},
    };
    for (const ToolInfo& info : toolInfo) {
        auto* button = new Button(Button::Kind::Glyph, toolbar);
        button->setGlyph(info.glyph);
        button->setCheckable(true);
        button->setAccessibleName(i18n::t(info.name));
        button->setToolTip(i18n::t(info.tip));
        const Tool tool = info.tool;
        connect(button, &QAbstractButton::clicked, this, [this, tool] { setTool(tool); });
        tools->addWidget(button);
        m_toolButtons.append(button);
    }
    auto* toolSeparator = new Separator(Qt::Vertical, toolbar);
    tools->addSpacing(6);
    tools->addWidget(toolSeparator);
    tools->addSpacing(6);
    m_undo = new Button(Button::Kind::Glyph, toolbar);
    m_undo->setGlyph(Glyph::Undo);
    m_undo->setAccessibleName(i18n::t("Undo"));
    m_undo->setToolTip(i18n::t("Undo (Ctrl+Z)"));
    connect(m_undo, &QAbstractButton::clicked, this, [this] { undo(); });
    m_redo = new Button(Button::Kind::Glyph, toolbar);
    m_redo->setGlyph(Glyph::Redo);
    m_redo->setAccessibleName(i18n::t("Redo"));
    m_redo->setToolTip(i18n::t("Redo (Ctrl+Y)"));
    connect(m_redo, &QAbstractButton::clicked, this, [this] { redo(); });
    tools->addWidget(m_undo);
    tools->addWidget(m_redo);
    tools->addStretch(1);
    m_cancel = new QPushButton(i18n::t("Cancel"), toolbar);
    m_done   = new QPushButton(i18n::t("Done"), toolbar);
    m_done->setDefault(true);
    m_done->setToolTip(i18n::t("Use the edited picture (Enter)"));
    m_cancel->setAutoDefault(false);
    connect(m_cancel, &QPushButton::clicked, this, [this] { reject(); });
    connect(m_done, &QPushButton::clicked, this, [this] { finish(); });
    tools->addWidget(m_cancel);
    tools->addWidget(m_done);

    // ---- options bar (changes with the tool) --------------------------------------------------------
    m_options = new QStackedWidget(this);
    m_options->setFixedHeight(kOptionsHeight);

    m_drawPage  = new QWidget(m_options);
    auto* draw  = new QHBoxLayout(m_drawPage);
    draw->setContentsMargins(12, 4, 12, 4);
    draw->setSpacing(8);
    for (int i = 0; i < imageedit::kColorCount; ++i) {
        auto* swatch = new Button(Button::Kind::Swatch, m_drawPage);
        swatch->setCheckable(true);
        swatch->setSwatch(imageedit::paletteColor(i));
        swatch->setAccessibleName(imageedit::paletteColorName(i));
        swatch->setToolTip(QStringLiteral("%1 (%2)").arg(imageedit::paletteColorName(i)).arg(i + 1));
        connect(swatch, &QAbstractButton::clicked, this, [this, i] { setColor(i); });
        draw->addWidget(swatch);
        m_swatches.append(swatch);
    }
    draw->addSpacing(8);
    draw->addWidget(new Separator(Qt::Vertical, m_drawPage));
    draw->addSpacing(8);
    const qreal dots[imageedit::kStrokeCount] = {4.0, 7.0, 11.0};
    for (int i = 0; i < imageedit::kStrokeCount; ++i) {
        auto* size = new Button(Button::Kind::Dot, m_drawPage);
        size->setCheckable(true);
        size->setDot(dots[i]);
        connect(size, &QAbstractButton::clicked, this, [this, i] { setStroke(i); });
        draw->addWidget(size);
        m_sizes.append(size);
    }
    draw->addStretch(1);

    m_cropPage = new QWidget(m_options);
    auto* crop = new QHBoxLayout(m_cropPage);
    crop->setContentsMargins(12, 4, 12, 4);
    crop->setSpacing(4);
    const char* aspectNames[] = {"Free", "Original", "Square", "4:3", "16:9"};
    for (int i = 0; i < 5; ++i) {
        auto* aspect = new Button(Button::Kind::Text, m_cropPage);
        aspect->setCheckable(true);
        aspect->setText(i18n::t(aspectNames[i]));
        aspect->setAccessibleName(i18n::t("Aspect ratio: %1").arg(i18n::t(aspectNames[i])));
        const auto value = static_cast<Aspect>(i);
        connect(aspect, &QAbstractButton::clicked, this, [this, value] { setAspect(value); });
        crop->addWidget(aspect);
        m_aspects.append(aspect);
    }
    crop->addSpacing(8);
    crop->addWidget(new Separator(Qt::Vertical, m_cropPage));
    crop->addSpacing(8);
    m_swap = new Button(Button::Kind::Text, m_cropPage);
    m_swap->setGlyph(Glyph::Swap);
    m_swap->setText(i18n::t("Swap"));
    m_swap->setToolTip(i18n::t("Swap portrait and landscape (X)"));
    connect(m_swap, &QAbstractButton::clicked, this, [this] { swapAspect(); });
    m_rotate = new Button(Button::Kind::Text, m_cropPage);
    m_rotate->setGlyph(Glyph::Rotate);
    m_rotate->setText(i18n::t("Rotate"));
    m_rotate->setToolTip(i18n::t("Rotate 90° clockwise (Ctrl+R)"));
    connect(m_rotate, &QAbstractButton::clicked, this, [this] { rotate(); });
    m_resetCrop = new Button(Button::Kind::Text, m_cropPage);
    m_resetCrop->setText(i18n::t("Reset crop"));
    m_resetCrop->setToolTip(i18n::t("Show the whole picture again"));
    connect(m_resetCrop, &QAbstractButton::clicked, this, [this] { resetCrop(); });
    crop->addWidget(m_swap);
    crop->addWidget(m_rotate);
    crop->addWidget(m_resetCrop);
    crop->addStretch(1);

    m_hidePage = new QWidget(m_options);
    auto* hide = new QHBoxLayout(m_hidePage);
    hide->setContentsMargins(12, 4, 12, 4);
    hide->setSpacing(4);
    const QString hideTip = i18n::t("Pixelate hides most details. For passwords and codes, use Black box.");
    const char*   modes[] = {"Pixelate", "Black box"};
    for (int i = 0; i < 2; ++i) {
        auto* mode = new Button(Button::Kind::Text, m_hidePage);
        mode->setCheckable(true);
        mode->setText(i18n::t(modes[i]));
        mode->setToolTip(hideTip);
        connect(mode, &QAbstractButton::clicked, this, [this, i] { setHideMode(i); });
        hide->addWidget(mode);
        m_hideModes.append(mode);
    }
    hide->addSpacing(12);
    m_hideHint = roleLabel("hint", m_hidePage);
    m_hideHint->setText(hideTip);
    hide->addWidget(m_hideHint, 1);

    m_options->addWidget(m_drawPage);
    m_options->addWidget(m_cropPage);
    m_options->addWidget(m_hidePage);

    // ---- canvas and status bar -------------------------------------------------------------------
    m_canvas = new Canvas(this);

    m_statusBar = new QWidget(this);
    m_statusBar->setFixedHeight(kStatusHeight);
    auto* status = new QHBoxLayout(m_statusBar);
    status->setContentsMargins(12, 0, 12, 0);
    status->setSpacing(12);
    m_errorIcon = new QLabel(m_statusBar);
    m_errorIcon->setFixedSize(16, 16);
    m_errorIcon->hide();
    m_errorText = roleLabel("error", m_statusBar);
    m_errorText->hide();
    m_sizeLabel = roleLabel("", m_statusBar);
    m_zoomLabel = roleLabel("hint", m_statusBar);
    m_hintLabel = roleLabel("hint", m_statusBar);
    m_hintLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_hintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status->addWidget(m_errorIcon);
    status->addWidget(m_errorText);
    status->addWidget(m_sizeLabel);
    status->addWidget(m_zoomLabel);
    status->addWidget(m_hintLabel, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(new Separator(Qt::Horizontal, this));
    layout->addWidget(m_options);
    layout->addWidget(new Separator(Qt::Horizontal, this));
    layout->addWidget(m_canvas, 1);
    layout->addWidget(new Separator(Qt::Horizontal, this));
    layout->addWidget(m_statusBar);

    // Tab order: tools, undo/redo, the options, the canvas, Cancel, Done.
    QWidget* previous = nullptr;
    const auto chain  = [&previous](QWidget* next) {
        if (previous)
            QWidget::setTabOrder(previous, next);
        previous = next;
    };
    for (Button* b : qAsConst(m_toolButtons))
        chain(b);
    chain(m_undo);
    chain(m_redo);
    for (Button* b : qAsConst(m_swatches))
        chain(b);
    for (Button* b : qAsConst(m_sizes))
        chain(b);
    for (Button* b : qAsConst(m_aspects))
        chain(b);
    chain(m_swap);
    chain(m_rotate);
    chain(m_resetCrop);
    for (Button* b : qAsConst(m_hideModes))
        chain(b);
    chain(m_canvas);
    chain(m_cancel);
    chain(m_done);

    // Dark skins switch focus indicators off: rings show once the keyboard moved the focus here.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (m_keyboardFocus || !now || now->window() != this || !testAttribute(Qt::WA_KeyboardFocusChange))
            return;
        m_keyboardFocus = true;
        applyTheme();
    });

    applyTheme();
    setTool(Tool::Pen);
    updateControls();
    updateStatus();
}

ImageEditor::~ImageEditor() = default;

QWidget* ImageEditor::canvasWidget() const
{
    return m_canvas;
}

imageedit::Prefs ImageEditor::prefs() const
{
    imageedit::Prefs prefs;
    prefs.color    = m_color;
    prefs.stroke   = m_stroke;
    prefs.hideMode = m_hideMode;
    return prefs;
}

// ---- loading and saving -------------------------------------------------------------------------

void ImageEditor::setModel(const imageedit::ImageEditModel& model)
{
    m_model       = model;
    m_openedDoc   = model.doc();
    m_pendingCrop = m_model.crop();
    m_loaded      = !m_model.base().isNull();
    m_canvas->setMessage(QString(), false);
    m_canvas->resetView();
    modelChanged();
    if (isVisible())
        m_canvas->setFocus(Qt::OtherFocusReason);
}

void ImageEditor::setOpenError(const QString& text)
{
    m_loaded = false;
    m_canvas->setMessage(text.endsWith(QLatin1Char('.')) ? text : text + QLatin1Char('.'), true);
    m_canvas->setAccessibleDescription(text);
    updateControls();
    updateStatus();
    QAccessibleEvent event(m_canvas, QAccessible::NameChanged);
    if (QAccessible::isActive())
        QAccessible::updateAccessibility(&event);
}

void ImageEditor::setSaving(bool saving)
{
    m_saving = saving;
    m_done->setText(saving ? i18n::t("Saving…") : i18n::t("Done"));
    if (saving) {
        m_errorIcon->hide();
        m_errorText->hide();
        m_sizeLabel->show();
    }
    m_canvas->setEnabled(!saving);
    updateControls();
    updateStatus();
}

void ImageEditor::setSaveError(const QString& text)
{
    setSaving(false);
    m_errorText->setText(text);
    m_errorText->show();
    m_errorIcon->show();
    m_sizeLabel->hide();
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_errorText, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

// ---- tools ----------------------------------------------------------------------------------------

void ImageEditor::setTool(Tool tool)
{
    if (tool == m_tool && m_options->currentWidget()) {
        updateControls();
        return;
    }
    m_canvas->commitText();
    if (m_tool == Tool::Crop && tool != Tool::Crop)
        applyCrop(); // leaving the crop tool keeps the crop
    m_tool = tool;
    if (tool == Tool::Crop)
        m_pendingCrop = m_model.crop();
    else
        m_drawTool = tool;
    m_options->setCurrentWidget(tool == Tool::Crop ? m_cropPage : tool == Tool::Hide ? m_hidePage : m_drawPage);
    m_canvas->toolChanged();
    updateControls();
    updateStatus();
}

void ImageEditor::setColor(int index)
{
    m_color = qBound(0, index, imageedit::kColorCount - 1);
    m_canvas->styleChanged();
    updateControls();
}

void ImageEditor::setStroke(int index)
{
    m_stroke = qBound(0, index, imageedit::kStrokeCount - 1);
    m_canvas->styleChanged();
    updateControls();
}

void ImageEditor::setHideMode(int mode)
{
    m_hideMode = qBound(0, mode, 1);
    updateControls();
}

double ImageEditor::aspectValue() const
{
    const QSize rotated = m_model.rotatedSize();
    double      value   = 0;
    switch (m_aspect) {
    case Aspect::Free:
        return 0;
    case Aspect::Original:
        value = rotated.height() > 0 ? double(rotated.width()) / rotated.height() : 1.0;
        break;
    case Aspect::Square:
        return 1.0;
    case Aspect::FourThree:
        value = 4.0 / 3.0;
        break;
    case Aspect::SixteenNine:
        value = 16.0 / 9.0;
        break;
    }
    return m_portrait ? 1.0 / value : value;
}

void ImageEditor::setAspect(Aspect aspect)
{
    if (!m_loaded)
        return;
    m_aspect = aspect;
    if (aspect == Aspect::Original)
        m_portrait = false;
    else if (aspect == Aspect::FourThree || aspect == Aspect::SixteenNine)
        m_portrait = m_model.rotatedSize().height() > m_model.rotatedSize().width(); // the picture's own way
    if (m_tool != Tool::Crop)
        setTool(Tool::Crop);
    if (aspect != Aspect::Free) {
        // The largest crop of that shape, around the middle of the current one.
        const QRect bounds(QPoint(0, 0), m_model.rotatedSize());
        const QRect current = imageedit::baseToRotated(m_pendingCrop, m_model.rotation(), m_model.baseSize());
        const QRect fitted  = imageedit::fitAspect(bounds, aspectValue(), QRectF(current).center());
        m_pendingCrop       = imageedit::rotatedToBase(fitted, m_model.rotation(), m_model.baseSize());
    }
    m_canvas->update();
    updateControls();
    updateStatus();
}

void ImageEditor::swapAspect()
{
    if (!m_loaded)
        return;
    if (m_tool != Tool::Crop)
        setTool(Tool::Crop);
    const QRect bounds(QPoint(0, 0), m_model.rotatedSize());
    const QRect current = imageedit::baseToRotated(m_pendingCrop, m_model.rotation(), m_model.baseSize());
    QRect       swapped;
    if (m_aspect == Aspect::Free) {
        // The same crop turned on its side, as far as the picture allows, around the same middle.
        QRect r(QPoint(0, 0), QSize(qMin(current.height(), bounds.width()), qMin(current.width(), bounds.height())));
        r.moveCenter(current.center());
        swapped = imageedit::dragCrop(r, imageedit::Handle::Move, QPointF(), bounds, 0, imageedit::kMinCropSide); // moved inside
    } else if (m_aspect != Aspect::Square) {
        m_portrait = !m_portrait;
        swapped    = imageedit::fitAspect(bounds, aspectValue(), QRectF(current).center());
    } else {
        return;
    }
    if (!swapped.isEmpty())
        m_pendingCrop = imageedit::rotatedToBase(swapped, m_model.rotation(), m_model.baseSize());
    m_canvas->update();
    updateControls();
    updateStatus();
}

void ImageEditor::rotate()
{
    if (!m_loaded || m_saving)
        return;
    m_canvas->commitText();
    m_model.rotateClockwise();
    // A shaped crop turns with the picture: 16:9 becomes 9:16.
    if (m_aspect == Aspect::FourThree || m_aspect == Aspect::SixteenNine || m_aspect == Aspect::Original)
        m_portrait = !m_portrait;
    m_canvas->fit();
    modelChanged();
}

void ImageEditor::applyCrop()
{
    if (!m_loaded || m_pendingCrop.isEmpty())
        return;
    if (m_pendingCrop != m_model.crop()) {
        m_model.setCrop(m_pendingCrop);
        modelChanged();
    }
}

void ImageEditor::cancelCrop()
{
    m_pendingCrop = m_model.crop();
    setTool(m_drawTool);
}

void ImageEditor::resetCrop()
{
    if (!m_loaded)
        return;
    if (m_tool != Tool::Crop)
        setTool(Tool::Crop);
    m_aspect      = Aspect::Free;
    m_pendingCrop = QRect(QPoint(0, 0), m_model.baseSize());
    m_canvas->update();
    updateControls();
    updateStatus();
}

void ImageEditor::nudgeCrop(int key, bool resize)
{
    const QRect bounds(QPoint(0, 0), m_model.rotatedSize());
    const QRect current = imageedit::baseToRotated(m_pendingCrop, m_model.rotation(), m_model.baseSize());
    const qreal step    = qMax(1.0, std::round(10.0 / qMax(0.0001, m_canvas->scale()))); // 10 px on screen
    QPointF     delta;
    switch (key) {
    case Qt::Key_Left:
        delta = QPointF(-step, 0);
        break;
    case Qt::Key_Right:
        delta = QPointF(step, 0);
        break;
    case Qt::Key_Up:
        delta = QPointF(0, -step);
        break;
    default:
        delta = QPointF(0, step);
        break;
    }
    const imageedit::Handle handle = !resize ? imageedit::Handle::Move : (delta.x() != 0 ? imageedit::Handle::Right : imageedit::Handle::Bottom);
    const QRect             moved  = imageedit::dragCrop(current, handle, delta, bounds, aspectValue(), imageedit::kMinCropSide);
    if (!moved.isEmpty())
        m_pendingCrop = imageedit::rotatedToBase(moved, m_model.rotation(), m_model.baseSize());
    m_canvas->update();
    updateControls();
    updateStatus();
}

void ImageEditor::undo()
{
    if (!m_loaded || m_saving)
        return;
    m_canvas->commitText();
    // With the crop tool, an unapplied crop is undone first.
    if (m_tool == Tool::Crop && m_pendingCrop != m_model.crop()) {
        m_pendingCrop = m_model.crop();
        m_canvas->update();
        updateControls();
        updateStatus();
        return;
    }
    if (m_model.undo()) {
        m_pendingCrop = m_model.crop();
        modelChanged();
    }
}

void ImageEditor::redo()
{
    if (!m_loaded || m_saving)
        return;
    m_canvas->commitText();
    if (m_model.redo()) {
        m_pendingCrop = m_model.crop();
        modelChanged();
    }
}

void ImageEditor::modelChanged()
{
    m_canvas->invalidate();
    updateControls();
    updateStatus();
}

void ImageEditor::finish()
{
    if (!m_loaded || m_saving)
        return;
    m_canvas->commitText();
    if (m_tool == Tool::Crop)
        setTool(m_drawTool); // applies the crop and shows the result while it is saved
    emit doneRequested();
}

bool ImageEditor::changedHere() const
{
    if (!m_loaded)
        return false;
    return m_model.doc() != m_openedDoc || (m_tool == Tool::Crop && m_pendingCrop != m_model.crop()) || m_canvas->hasText();
}

void ImageEditor::reject()
{
    if (m_saving)
        return; // the copy is being written; it answers in a moment
    if (m_discard) {
        m_discard->raise();
        m_discard->activateWindow();
        return;
    }
    if (!changedHere()) {
        QDialog::reject();
        return;
    }
    // Not exec(): no nested event loop (plugin shutdown may delete this window at any time).
    auto* box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setWindowTitle(windowTitle());
    box->setTextFormat(Qt::PlainText);
    box->setStyleSheet(QString::fromLatin1("QLabel{min-width:260px;}"));
    box->setText(i18n::t("Discard your changes to this image?"));
    QPushButton* discard = box->addButton(i18n::t("Discard"), QMessageBox::DestructiveRole);
    QPushButton* keep    = box->addButton(i18n::t("Keep editing"), QMessageBox::RejectRole);
    box->setDefaultButton(keep);
    box->setEscapeButton(keep);
    connect(box, &QMessageBox::finished, this, [this, box, discard] {
        if (box->clickedButton() == discard)
            QDialog::reject();
    });
    m_discard = box;
    box->open();
}

// ---- keys -------------------------------------------------------------------------------------------

void ImageEditor::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers mods  = event->modifiers() & ~Qt::KeypadModifier;
    const bool                  ctrl  = mods == Qt::ControlModifier;
    const bool                  plain = mods == Qt::NoModifier;
    const quint32               vk    = event->nativeVirtualKey();
    const int                   key   = event->key();
    const auto                  is    = [key, vk](Qt::Key k, quint32 v) { return key == k || vk == v; };

    if (key == Qt::Key_Escape) {
        if (m_canvas->cancelInteraction())
            return;
        if (m_loaded && !m_saving && m_tool == Tool::Crop) {
            cancelCrop();
            return;
        }
        reject();
        return;
    }
    if (!m_loaded || m_saving) {
        QDialog::keyPressEvent(event);
        return;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (plain || ctrl)) {
        if (m_tool == Tool::Crop && plain)
            setTool(m_drawTool); // applies the crop
        else
            finish();
        return;
    }
    if (ctrl && is(Qt::Key_Z, kVkZ)) {
        undo();
        return;
    }
    if ((ctrl && is(Qt::Key_Y, kVkY)) || (mods == (Qt::ControlModifier | Qt::ShiftModifier) && is(Qt::Key_Z, kVkZ))) {
        redo();
        return;
    }
    if (ctrl && is(Qt::Key_0, kVk0)) {
        m_canvas->fit();
        return;
    }
    if (ctrl && is(Qt::Key_1, kVk1)) {
        m_canvas->actualSize();
        return;
    }
    if (ctrl && is(Qt::Key_R, kVkR)) {
        rotate();
        return;
    }
    if (ctrl && (key == Qt::Key_Plus || key == Qt::Key_Equal || key == Qt::Key_Minus)) {
        m_canvas->zoomAt(key == Qt::Key_Minus ? 1 / 1.25 : 1.25, QPointF(m_canvas->width() / 2.0, m_canvas->height() / 2.0));
        return;
    }
    if (m_tool == Tool::Crop && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)
        && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down)) {
        nudgeCrop(key, mods == Qt::ShiftModifier); // the keyboard way to crop: arrows move, Shift+arrows resize
        return;
    }
    if (plain) {
        if (is(Qt::Key_C, kVkC)) {
            setTool(Tool::Crop);
            return;
        }
        if (is(Qt::Key_P, kVkP)) {
            setTool(Tool::Pen);
            return;
        }
        if (is(Qt::Key_A, kVkA)) {
            setTool(Tool::Arrow);
            return;
        }
        if (is(Qt::Key_R, kVkR)) {
            setTool(Tool::Rect);
            return;
        }
        if (is(Qt::Key_T, kVkT)) {
            setTool(Tool::Text);
            return;
        }
        if (is(Qt::Key_H, kVkH)) {
            setTool(Tool::Hide);
            return;
        }
        if (is(Qt::Key_X, kVkX) && m_tool == Tool::Crop) {
            swapAspect();
            return;
        }
        if (vk >= kVk1 && vk < kVk1 + imageedit::kColorCount) {
            setColor(static_cast<int>(vk - kVk1));
            return;
        }
        if (key >= Qt::Key_1 && key < Qt::Key_1 + imageedit::kColorCount) {
            setColor(key - Qt::Key_1);
            return;
        }
        if (is(Qt::Key_BracketLeft, kVkOpenBracket)) {
            setStroke(m_stroke - 1);
            return;
        }
        if (is(Qt::Key_BracketRight, kVkCloseBracket)) {
            setStroke(m_stroke + 1);
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

// ---- state shown --------------------------------------------------------------------------------------

void ImageEditor::updateControls()
{
    const bool on = m_loaded && !m_saving;
    for (int i = 0; i < m_toolButtons.size(); ++i) {
        m_toolButtons.at(i)->setEnabled(on);
        m_toolButtons.at(i)->setChecked(static_cast<int>(m_tool) == i);
    }
    const bool pendingCrop = m_tool == Tool::Crop && m_pendingCrop != m_model.crop();
    m_undo->setEnabled(on && (m_model.canUndo() || pendingCrop));
    m_redo->setEnabled(on && m_model.canRedo());
    m_done->setEnabled(on);
    m_cancel->setEnabled(!m_saving);
    m_options->setEnabled(on);
    for (int i = 0; i < m_swatches.size(); ++i)
        m_swatches.at(i)->setChecked(i == m_color);
    const bool text = m_tool == Tool::Text;
    const char* textNames[]   = {"Small text", "Medium text", "Large text"};
    const char* strokeNames[] = {"Thin line", "Medium line", "Thick line"};
    for (int i = 0; i < m_sizes.size(); ++i) {
        Button* size = m_sizes.at(i);
        size->setChecked(i == m_stroke);
        const QString name = i18n::t(text ? textNames[i] : strokeNames[i]);
        size->setAccessibleName(name);
        size->setToolTip(QStringLiteral("%1 (%2)").arg(name, i18n::t("[ and ]")));
    }
    for (int i = 0; i < m_aspects.size(); ++i)
        m_aspects.at(i)->setChecked(static_cast<int>(m_aspect) == i);
    m_swap->setEnabled(on && m_aspect != Aspect::Square);
    m_resetCrop->setEnabled(on && m_tool == Tool::Crop && m_pendingCrop != QRect(QPoint(0, 0), m_model.baseSize()));
    for (int i = 0; i < m_hideModes.size(); ++i)
        m_hideModes.at(i)->setChecked(i == m_hideMode);
}

QString ImageEditor::hintText() const
{
    QString tool;
    if (m_canvas->textOpen())
        tool = i18n::t("Enter adds the text · Esc cancels it");
    else {
        switch (m_tool) {
        case Tool::Crop:
            tool = i18n::t("Drag the frame or its corners · Arrows move it, Shift+arrows resize · Enter applies");
            break;
        case Tool::Pen:
            tool = i18n::t("Drag to draw · Shift: straight line");
            break;
        case Tool::Arrow:
            tool = i18n::t("Drag from the tail to the tip · Shift: 45° steps");
            break;
        case Tool::Rect:
            tool = i18n::t("Drag to draw a frame · Shift: square");
            break;
        case Tool::Text:
            tool = i18n::t("Click where the text should start");
            break;
        case Tool::Hide:
            tool = i18n::t("Drag over what to hide");
            break;
        }
    }
    return tool + QStringLiteral(" · ") + i18n::t("Ctrl+wheel to zoom · Space+drag to pan");
}

void ImageEditor::updateStatus()
{
    if (!m_loaded) {
        m_sizeLabel->clear();
        m_zoomLabel->clear();
        m_hintLabel->clear();
        return;
    }
    QString size;
    if (m_tool == Tool::Crop) {
        const QRect crop = imageedit::baseToRotated(m_pendingCrop, m_model.rotation(), m_model.baseSize());
        size             = i18n::t("Crop: %1 × %2 px").arg(crop.width()).arg(crop.height());
    } else {
        const QSize out = m_model.outputSize();
        size            = i18n::t("%1 × %2 px").arg(out.width()).arg(out.height());
    }
    m_sizeLabel->setText(size);
    m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_canvas->zoomFactor() * 100)));
    m_zoomLabel->setToolTip(i18n::t("Zoom: Ctrl+0 fits the window, Ctrl+1 shows actual size"));
    const QString hint = hintText();
    m_hintLabel->setText(m_hintLabel->fontMetrics().elidedText(hint, Qt::ElideRight, qMax(40, m_hintLabel->width())));
    m_hintLabel->setToolTip(hint);
    m_canvas->setAccessibleDescription(size + QStringLiteral(". ") + hint);
}

void ImageEditor::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    m_canvas->setFocus(Qt::OtherFocusReason);
    updateStatus();
}

void ImageEditor::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        applyTheme();
}

void ImageEditor::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    ensurePolished();
    const Colors colors = editorui::colorsFor(palette());
    QVector<Button*> buttons = m_toolButtons;
    buttons << m_undo << m_redo << m_swatches << m_sizes << m_aspects << m_swap << m_rotate << m_resetCrop << m_hideModes;
    for (Button* b : qAsConst(buttons))
        b->setColors(colors);
    for (QWidget* w : findChildren<QWidget*>())
        w->update(); // the separators
    m_canvas->setColors(colors);
    colorLabel(m_hideHint, colors.muted);
    colorLabel(m_zoomLabel, colors.muted);
    colorLabel(m_hintLabel, colors.muted);
    colorLabel(m_errorText, colors.error);
    const qreal dpr = devicePixelRatioF();
    QPixmap     alert(QSize(16, 16) * dpr);
    alert.setDevicePixelRatio(dpr);
    alert.fill(Qt::transparent);
    {
        QPainter p(&alert);
        editorui::drawAlert(p, QRectF(0, 0, 16, 16), colors.error, colors.window);
    }
    m_errorIcon->setPixmap(alert);

    QString sheet;
    if (!qApp->styleSheet().isEmpty()) {
        // TeamSpeak's skins colour labels by style sheet, which beats a palette. fromLatin1, never
        // QStringLiteral: TeamSpeak's style keeps the parsed text after the plugin is unloaded.
        sheet = QString::fromLatin1("#tsmediaImageEditor QLabel[role=\"hint\"]{color:%1;}"
                                    "#tsmediaImageEditor QLabel[role=\"error\"]{color:%2;}")
                    .arg(colors.muted.name(), colors.error.name());
        if (colors.dark && m_keyboardFocus)
            sheet += QString::fromLatin1("#tsmediaImageEditor QPushButton:focus{border:2px solid #ffffff;padding:6px 12px;}");
    }
    if (sheet != styleSheet())
        setStyleSheet(sheet);
    m_applyingTheme = false;
}
