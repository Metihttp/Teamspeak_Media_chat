#include "emojiinput.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAction>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QRandomGenerator>
#include <QTextEdit>

#include <algorithm>

#include "emojidata.h"
#include "emojipicker.h"
#include "emojirender.h"
#include "i18n.h"
#include "settings.h"

namespace {

constexpr int kButton = 24; // logical px
constexpr int kMargin = 30; // the input's viewport gives this much room at the right

// setViewportMargins / viewportMargins are protected: reached through a derived class's pointers to
// member (a legal way to call them on TeamSpeak's input).
struct ScrollAreaAccess : public QAbstractScrollArea {
    static void set(QAbstractScrollArea* area, const QMargins& margins)
    {
        const auto setter = static_cast<void (QAbstractScrollArea::*)(const QMargins&)>(&ScrollAreaAccess::setViewportMargins);
        (area->*setter)(margins);
    }
    static QMargins get(QAbstractScrollArea* area)
    {
        const auto getter = &ScrollAreaAccess::viewportMargins;
        return (area->*getter)();
    }
};

bool isDark(const QWidget* w)
{
    const QPalette pal = w->palette();
    return pal.color(QPalette::Base).lightness() < 128 || pal.color(QPalette::Text).lightness() > 170;
}

QImage greyed(const QImage& source, int alpha)
{
    QImage out = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < out.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int a = qAlpha(line[x]) * alpha / 255;
            const int g = qGray(line[x]) * alpha / 255;
            line[x]     = qRgba(g, g, g, a);
        }
    }
    out.setDevicePixelRatio(source.devicePixelRatio());
    return out;
}

// Faces the button shows under the pointer (one at random each time, like Discord).
int randomFace()
{
    static const char* const faces[] = {"1f600", "1f603", "1f604", "1f601", "1f606", "1f60a", "1f60e", "1f929", "1f973", "1f61c", "1f917", "1f642"};
    const int index = static_cast<int>(QRandomGenerator::global()->bounded(static_cast<int>(sizeof(faces) / sizeof(faces[0]))));
    return emoji::fromWireCode(QByteArray(faces[index]));
}

class EmojiButton : public QAbstractButton
{
  public:
    explicit EmojiButton(QWidget* parent)
        : QAbstractButton(parent)
    {
        // Named like the plugin's other widgets inside TeamSpeak's, so shutdown's sweep finds it.
        setObjectName(QString::fromLatin1("tsmediaEmojiButton"));
        setFocusPolicy(Qt::NoFocus); // typing stays in the input; Ctrl+E is the keyboard way
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setAccessibleName(i18n::t("Emoji"));
        setToolTip(i18n::t("Emoji (Ctrl+E)"));
        m_face = randomFace();
    }

    // The strip right of the text, as high as the viewport; the icon in its middle (or at the bottom of a
    // tall input).
    void place(const QRect& strip)
    {
        setGeometry(strip);
        m_icon = strip.height() <= 44 ? QPoint((strip.width() - kButton) / 2, (strip.height() - kButton) / 2)
                                      : QPoint((strip.width() - kButton) / 2, strip.height() - kButton - 3);
        update();
    }

  protected:
    void enterEvent(QEvent* event) override
    {
        m_face = randomFace();
        QAbstractButton::enterEvent(event);
    }

    bool hitButton(const QPoint& pos) const override { return QRect(m_icon, QSize(kButton, kButton)).adjusted(-3, -3, 3, 3).contains(pos); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // A native input paints only its viewport white: the strip gets the same. With TeamSpeak's style
        // sheets the input's background already covers it (the viewport is transparent then).
        auto* area = qobject_cast<QAbstractScrollArea*>(parentWidget());
        if (area && area->viewport()->autoFillBackground())
            p.fillRect(rect(), area->viewport()->palette().brush(area->viewport()->backgroundRole()));
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal  dpr = devicePixelRatioF();
        const QRectF box(m_icon, QSizeF(kButton, kButton));
        if (underMouse() || isDown()) {
            const qreal  size  = isDown() ? 20 : 22; // a little larger under the pointer, back on press
            const QImage image = emoji::render(m_face, 22, dpr);
            p.drawImage(QRectF(box.center().x() - size / 2, box.center().y() - size / 2, size, size), image);
        } else {
            const int    smile = emoji::fromWireCode("1f642");
            const QImage image = greyed(emoji::render(smile, 20, dpr), isDark(parentWidget() ? parentWidget() : this) ? 200 : 150);
            p.drawImage(QRectF(box.center().x() - 10, box.center().y() - 10, 20, 20), image);
        }
    }

  private:
    int    m_face = -1;
    QPoint m_icon;
};

QIcon pickerIcon(qreal dpr, bool dark)
{
    QIcon     icon;
    const int smile = emoji::fromWireCode("1f642");
    icon.addPixmap(QPixmap::fromImage(greyed(emoji::render(smile, 16, dpr), dark ? 200 : 150)), QIcon::Normal);
    icon.addPixmap(QPixmap::fromImage(emoji::render(smile, 16, dpr)), QIcon::Active);
    return icon;
}

} // namespace

EmojiInput::EmojiInput(QObject* parent)
    : QObject(parent)
{
}

EmojiInput::~EmojiInput()
{
    if (m_picker)
        delete m_picker.data();
    for (Input& input : m_inputs)
        detach(input);
}

void EmojiInput::attach(QTextEdit* edit)
{
    if (!edit)
        return;
    for (const Input& input : qAsConst(m_inputs)) {
        if (input.edit == edit)
            return;
    }
    m_inputs.erase(std::remove_if(m_inputs.begin(), m_inputs.end(), [](const Input& i) { return i.edit.isNull(); }), m_inputs.end());
    Input input;
    input.edit = edit;
    edit->installEventFilter(this);
    m_inputs.append(input);
    layout(m_inputs.last());
}

void EmojiInput::detach(Input& input)
{
    if (input.edit) {
        input.edit->removeEventFilter(this);
        if (input.ours)
            ScrollAreaAccess::set(input.edit, input.original);
    }
    input.ours = false;
    if (input.button)
        delete input.button.data();
}

EmojiInput::Input* EmojiInput::inputFor(QObject* watched)
{
    for (Input& input : m_inputs) {
        if (input.edit && input.edit.data() == watched)
            return &input;
    }
    return nullptr;
}

void EmojiInput::settingsChanged()
{
    for (Input& input : m_inputs)
        layout(input);
}

void EmojiInput::layout(Input& input)
{
    QTextEdit* edit = input.edit;
    if (!edit)
        return;
    const bool wanted = Settings::instance().emojiButton;
    if (!wanted) {
        if (input.button)
            input.button->hide();
        if (input.ours) {
            ScrollAreaAccess::set(edit, input.original);
            input.ours = false;
        }
        return;
    }
    if (!input.ours) {
        input.original = ScrollAreaAccess::get(edit);
        input.ours     = true;
    }
    const QMargins mine(input.original.left(), input.original.top(), input.original.right() + kMargin, input.original.bottom());
    if (ScrollAreaAccess::get(edit) != mine)
        ScrollAreaAccess::set(edit, mine);
    if (!input.button) {
        auto* button = new EmojiButton(edit);
        connect(button, &QAbstractButton::clicked, this, [this, guard = QPointer<QTextEdit>(edit)] {
            if (guard)
                openPicker(guard.data());
        });
        input.button = button;
    }
    // In the margin, right of the text: centred on a one-line input, at the bottom of a taller one.
    const QRect area = edit->viewport()->geometry();
    static_cast<EmojiButton*>(input.button.data())->place(QRect(area.right() + 1, area.top(), kMargin, area.height()));
    input.button->show();
    input.button->raise();
}

bool EmojiInput::hasInput() const
{
    for (const Input& input : m_inputs) {
        if (input.edit)
            return true;
    }
    return false;
}

void EmojiInput::insertEmoji(int id)
{
    QTextEdit* target = m_last && m_last->isVisible() ? m_last.data() : nullptr;
    for (const Input& input : m_inputs) {
        if (!target && input.edit && input.edit->isVisible())
            target = input.edit;
    }
    if (!target || !emoji::isValid(id))
        return;
    target->textCursor().insertText(emoji::text(id));
    target->setFocus(Qt::OtherFocusReason);
}

void EmojiInput::openPicker(QTextEdit* edit)
{
    if (!edit)
        return;
    if (m_picker) {
        m_picker->close(); // the button again: closes it
        return;
    }
    m_last          = edit;
    const bool dark = isDark(edit);
    auto*      picker = new EmojiPicker(EmojiPicker::Mode::Insert, dark, edit->palette().color(QPalette::Base), edit);
    m_picker          = picker;
    QPointer<QTextEdit> guard(edit);
    connect(picker, &EmojiPicker::picked, this, [guard](int id, bool) {
        if (!guard)
            return;
        QTextCursor cursor = guard->textCursor();
        cursor.insertText(emoji::text(id));
        guard->setTextCursor(cursor);
    });
    connect(picker, &EmojiPicker::closed, this, [guard] {
        if (guard)
            guard->setFocus(Qt::PopupFocusReason);
    });
    QRect anchor;
    for (const Input& input : qAsConst(m_inputs)) {
        if (input.edit == edit && input.button && input.button->isVisible())
            anchor = QRect(input.button->mapToGlobal(QPoint(0, 0)), input.button->size());
    }
    if (anchor.isNull())
        anchor = QRect(edit->mapToGlobal(QPoint(0, 0)), edit->size());
    picker->openAt(anchor);
}

bool EmojiInput::eventFilter(QObject* watched, QEvent* event)
{
    Input* input = inputFor(watched);
    if (!input)
        return false;
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::LayoutRequest:
        layout(*input);
        break;
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress: {
        auto*      ke   = static_cast<QKeyEvent*>(event);
        const bool ctrl = (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier)) == Qt::ControlModifier;
        // Ctrl+E, also on other keyboard layouts (the key, not the letter).
        if (ctrl && (ke->key() == Qt::Key_E || ke->nativeVirtualKey() == 'E')) {
            if (event->type() == QEvent::ShortcutOverride) {
                ke->accept();
                return true;
            }
            openPicker(input->edit.data());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return false;
}

void EmojiInput::addPickerButton(QLineEdit* field)
{
    if (!field)
        return;
    QAction* action = field->addAction(pickerIcon(field->devicePixelRatioF(), isDark(field)), QLineEdit::TrailingPosition);
    action->setToolTip(i18n::t("Emoji"));
    action->setObjectName(QString::fromLatin1("tsmediaEmojiAction"));
    QPointer<QLineEdit> guard(field);
    QObject::connect(action, &QAction::triggered, action, [guard] {
        if (!guard)
            return;
        auto* picker = new EmojiPicker(EmojiPicker::Mode::Insert, isDark(guard.data()), guard->palette().color(QPalette::Base), guard.data());
        QObject::connect(picker, &EmojiPicker::picked, guard.data(), [guard](int id, bool) {
            if (guard)
                guard->insert(emoji::text(id));
        });
        QObject::connect(picker, &EmojiPicker::closed, guard.data(), [guard] {
            if (guard)
                guard->setFocus(Qt::PopupFocusReason);
        });
        // Below the field (the send window is in the middle of the screen).
        picker->openAt(QRect(guard->mapToGlobal(QPoint(0, 0)), guard->size()), true);
    });
}
