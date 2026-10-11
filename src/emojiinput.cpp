#include "emojiinput.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStyleOption>
#include <QStylePainter>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>

#include "chatinput.h"
#include "emojidata.h"
#include "emojipicker.h"
#include "emojirender.h"
#include "i18n.h"
#include "micbutton.h" // 2.2.1 mic
#include "settings.h"

namespace {

constexpr int kButton = 24; // logical px (the fallback)
constexpr int kMargin = 30; // the input's viewport gives this much room at the right per slot (reserveSlots)
constexpr int kMicSlots = 1; // 2.2.1 mic: slot 0 is always the microphone button's (micbutton.h)
constexpr int kMaxFallbackButton = 48; // the last discovery rule: a button no larger than this
constexpr int kIdleAlpha     = 230;
constexpr int kDisabledAlpha = 110;

enum FaceMode { kIdle = 0, kColour = 1, kDisabled = 2 };

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

// The buttons' own protected initStyleOption, the same way: the option then has TeamSpeak's geometry,
// auto-raise, palette and whatever else the button puts in it, so the skin draws it as its own.
struct ToolButtonAccess : public QToolButton {
    static void init(const QToolButton* button, QStyleOptionToolButton* option)
    {
        const auto fn = &ToolButtonAccess::initStyleOption;
        (button->*fn)(option);
    }
};
struct PushButtonAccess : public QPushButton {
    static void init(const QPushButton* button, QStyleOptionButton* option)
    {
        const auto fn = &PushButtonAccess::initStyleOption;
        (button->*fn)(option);
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

// The face in TeamSpeak's icon tone: its shading kept (grey levels relative to the face's mean), its mean
// becomes tone; alpha scales the coverage.
QImage toned(const QImage& source, const QColor& tone, int alpha)
{
    QImage in = source.convertToFormat(QImage::Format_ARGB32);
    double sum = 0;
    int    n   = 0;
    for (int y = 0; y < in.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        for (int x = 0; x < in.width(); ++x) {
            if (qAlpha(line[x]) >= 160) {
                sum += qGray(line[x]);
                ++n;
            }
        }
    }
    const double mean = n > 0 && sum > 0 ? sum / n : 200.0;
    QImage       out(in.size(), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(source.devicePixelRatio());
    for (int y = 0; y < in.height(); ++y) {
        const auto* src = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        auto*       dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < in.width(); ++x) {
            const double shade = qMin(1.25, qGray(src[x]) / mean);
            const int    a     = qAlpha(src[x]) * alpha / 255;
            const int    r     = qBound(0, static_cast<int>(std::lround(tone.red() * shade)), 255);
            const int    g     = qBound(0, static_cast<int>(std::lround(tone.green() * shade)), 255);
            const int    b     = qBound(0, static_cast<int>(std::lround(tone.blue() * shade)), 255);
            dst[x]             = qPremultiply(qRgba(r, g, b, a));
        }
    }
    return out;
}

int smileFace()
{
    return emoji::fromWireCode(QByteArray("1f642")); // slightly smiling face
}

// Faces the button shows under the pointer (one at random each time, like Discord).
int randomFace()
{
    // Plain round faces only: busy ones (party hat, star eyes, hands) turn to noise at 16 px.
    static const char* const faces[] = {"1f600", "1f603", "1f604", "1f601", "1f606", "1f60a", "1f60e", "1f60b", "1f60d", "1f61c", "1f642"};
    const int index = static_cast<int>(QRandomGenerator::global()->bounded(static_cast<int>(sizeof(faces) / sizeof(faces[0]))));
    const int id    = emoji::fromWireCode(QByteArray(faces[index]));
    return emoji::isValid(id) ? id : smileFace();
}

bool isOurs(const QWidget* w)
{
    return w->objectName().startsWith(QLatin1String("tsmedia"));
}

bool inEmoticonsDisplay(const QWidget* w)
{
    // TeamSpeak's emoticon popup may reuse the button's name for its cells.
    for (const QWidget* p = w; p; p = p->parentWidget()) {
        if (p->inherits("EmoticonsDisplay"))
            return true;
    }
    return false;
}

bool supported(const QAbstractButton* button)
{
    return qobject_cast<const QToolButton*>(button) || qobject_cast<const QPushButton*>(button);
}

// TeamSpeak's button can be drawn on: of a class we draw like it, and not hidden on purpose (TeamSpeak's
// emoticon option; a hidden ancestor doesn't count, the input is hidden with it then).
bool usable(const QAbstractButton* button)
{
    return button && supported(button) && !button->isHidden();
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
            const QImage image = greyed(emoji::render(smileFace(), 20, dpr), isDark(parentWidget() ? parentWidget() : this) ? 200 : 150);
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
    const int smile = smileFace();
    icon.addPixmap(QPixmap::fromImage(greyed(emoji::render(smile, 16, dpr), dark ? 200 : 150)), QIcon::Normal);
    icon.addPixmap(QPixmap::fromImage(emoji::render(smile, 16, dpr)), QIcon::Active);
    return icon;
}

// One picture for every mode and state, so the style draws exactly it (no generated disabled look).
QIcon iconOf(const QPixmap& pixmap)
{
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
        icon.addPixmap(pixmap, mode, QIcon::Off);
        icon.addPixmap(pixmap, mode, QIcon::On);
    }
    return icon;
}

} // namespace

EmojiInput::EmojiInput(QObject* parent)
    : QObject(parent)
    , m_text(new ChatInputs(this))
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
    // The viewport is resized after the input's own Resize event (the scroll area lays it out then):
    // the button follows the viewport's.
    edit->viewport()->installEventFilter(this);
    m_inputs.append(input);
    m_text->attach(edit); // 2.2.1: TeamSpeak's placeholder, learned as it comes and goes
    layout(m_inputs.last());
}

void EmojiInput::rediscover()
{
    for (Input& input : m_inputs) {
        if (input.edit && !input.taken.button)
            layout(input);
    }
}

void EmojiInput::detach(Input& input)
{
    release(input);
    if (input.edit) {
        m_text->detach(input.edit);
        input.edit->removeEventFilter(this);
        input.edit->viewport()->removeEventFilter(this);
    }
    // Only here (unload, or the input is gone) do both buttons go and the input get its width back.
    if (input.button)
        delete input.button.data();
    MicButton::removeFrom(input.edit); // 2.2.1 mic
    reserveSlots(input, 0);
}

EmojiInput::Input* EmojiInput::inputFor(QObject* watched)
{
    for (Input& input : m_inputs) {
        if (input.edit && input.edit.data() == watched)
            return &input;
    }
    return nullptr;
}

EmojiInput::Input* EmojiInput::inputForButton(QObject* watched)
{
    for (Input& input : m_inputs) {
        if (input.taken.button && input.taken.button.data() == watched)
            return &input;
    }
    return nullptr;
}

void EmojiInput::settingsChanged()
{
    for (Input& input : m_inputs)
        layout(input);
}

// ---- discovery ---------------------------------------------------------------------------------------

QAbstractButton* EmojiInput::findTeamSpeakButton(QTextEdit* edit)
{
    if (!edit)
        return nullptr;
    QWidget* window = edit->window();
    QList<QAbstractButton*> candidates;
    // 1. By its name (ts3client: objectName "EmoticonButton", slot onEmoticonsButtonClicked).
    for (QAbstractButton* b : window->findChildren<QAbstractButton*>(QString::fromLatin1("EmoticonButton"))) {
        if (!inEmoticonsDisplay(b) && !isOurs(b))
            candidates.append(b);
    }
    // 2. Next to the input, by a name or tool tip with "emoticon" in it ("Show Emoticons").
    if (candidates.isEmpty() && edit->parentWidget()) {
        for (QAbstractButton* b : edit->parentWidget()->findChildren<QAbstractButton*>()) {
            if (inEmoticonsDisplay(b) || isOurs(b) || edit->isAncestorOf(b))
                continue;
            if (b->objectName().contains(QLatin1String("moticon"), Qt::CaseInsensitive) || b->toolTip().contains(QLatin1String("moticon"), Qt::CaseInsensitive))
                candidates.append(b);
        }
    }
    const QRect input(edit->mapTo(window, QPoint(0, 0)), edit->size());
    // 3. The nearest small visible button right of the input, on its line.
    if (candidates.isEmpty()) {
        for (QAbstractButton* b : window->findChildren<QAbstractButton*>()) {
            if (!b->isVisible() || inEmoticonsDisplay(b) || isOurs(b) || edit->isAncestorOf(b) || b->width() > kMaxFallbackButton || b->height() > kMaxFallbackButton)
                continue;
            const QRect r(b->mapTo(window, QPoint(0, 0)), b->size());
            if (r.left() < input.right() - 2 || r.bottom() < input.top() || r.top() > input.bottom())
                continue;
            candidates.append(b);
        }
    }
    QAbstractButton* best     = nullptr;
    qint64           bestDist = 0;
    const QPoint     edge(input.right(), input.center().y());
    for (QAbstractButton* b : qAsConst(candidates)) {
        const QRect  r(b->mapTo(window, QPoint(0, 0)), b->size());
        const QPoint d = r.center() - edge;
        const qint64 dist = static_cast<qint64>(d.x()) * d.x() + static_cast<qint64>(d.y()) * d.y();
        if (!best || dist < bestDist) {
            best     = b;
            bestDist = dist;
        }
    }
    return best;
}

QAbstractButton* EmojiInput::takenButton(QTextEdit* edit) const
{
    for (const Input& input : m_inputs) {
        if (input.edit == edit)
            return input.taken.button.data();
    }
    return nullptr;
}

QAbstractButton* EmojiInput::fallbackButton(QTextEdit* edit) const
{
    for (const Input& input : m_inputs) {
        if (input.edit == edit && input.button && !input.button->isHidden())
            return input.button.data();
    }
    return nullptr;
}

bool EmojiInput::pickerOpen() const
{
    return m_picker && m_picker->isVisible();
}

QString EmojiInput::describe() const
{
    QStringList lines;
    for (const Input& input : m_inputs) {
        if (!input.edit)
            continue;
        QAbstractButton* found = findTeamSpeakButton(input.edit);
        QString          line  = QString::fromLatin1("input %1: ").arg(input.edit->objectName());
        if (found) {
            QStringList chain;
            for (QWidget* w = found->parentWidget(); w && chain.size() < 6; w = w->parentWidget())
                chain << QString::fromLatin1(w->metaObject()->className()) + (w->objectName().isEmpty() ? QString() : QLatin1Char('#') + w->objectName());
            const auto* tool = qobject_cast<QToolButton*>(found);
            line += QString::fromLatin1("found %1#%2 in %3, %4,%5 %6x%7, icon %8x%9, %10 hidden %11 visible %12 focus %13 tip \"%14\"")
                        .arg(QString::fromLatin1(found->metaObject()->className()), found->objectName(), chain.join(QLatin1String(" < ")))
                        .arg(found->x())
                        .arg(found->y())
                        .arg(found->width())
                        .arg(found->height())
                        .arg(found->iconSize().width())
                        .arg(found->iconSize().height())
                        .arg(tool ? QString::fromLatin1("style %1 autoRaise %2 popup %3").arg(tool->toolButtonStyle()).arg(tool->autoRaise() ? 1 : 0).arg(tool->popupMode())
                                  : QString::fromLatin1("push button"))
                        .arg(found->isHidden() ? 1 : 0)
                        .arg(found->isVisible() ? 1 : 0)
                        .arg(found->focusPolicy())
                        .arg(input.taken.button ? input.taken.toolTip : found->toolTip());
        } else {
            line += QString::fromLatin1("no TeamSpeak button found");
        }
        line += QString::fromLatin1("; taken %1, fallback %2, picker %3")
                    .arg(input.taken.button ? 1 : 0)
                    .arg(input.button && !input.button->isHidden() ? 1 : 0)
                    .arg(pickerOpen() ? 1 : 0);
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

// ---- takeover ----------------------------------------------------------------------------------------

bool EmojiInput::takeOver(Input& input, QAbstractButton* button)
{
    if (!button || !supported(button))
        return false;
    if (input.taken.button == button)
        return true;
    release(input);
    Taken& t   = input.taken;
    t          = Taken();
    t.button   = button;
    t.toolTip  = button->toolTip();
    t.accessibleName        = button->accessibleName();
    t.accessibleDescription = button->accessibleDescription();
    t.face                  = randomFace();
    // i18n::t: heap strings (TeamSpeak's widget keeps them after we are gone if something goes wrong).
    button->setToolTip(i18n::t("Emoji (Ctrl+E)"));
    button->setAccessibleName(i18n::t("Emoji"));
    button->setAccessibleDescription(i18n::t("Opens the emoji picker"));
    button->installEventFilter(this);
    sampleTone(t);
    // Should never fire (the left press never reaches it); an accessibility press or a direct call could.
    // Then TeamSpeak's own handler ran too: on the next turn of the event loop its popup goes and ours opens.
    const QPointer<QTextEdit> edit = input.edit;
    t.pressed = connect(button, &QAbstractButton::pressed, this, [this, edit] { safetyNet(edit); });
    t.clicked = connect(button, &QAbstractButton::clicked, this, [this, edit] { safetyNet(edit); });
    // TeamSpeak deleted it (a skin or layout change): the fallback, or a new button, on the next turn.
    t.destroyed = connect(button, &QObject::destroyed, this, [this, edit] {
        QTimer::singleShot(0, this, [this, edit] {
            if (Input* in = inputFor(edit.data()))
                layout(*in);
        });
    });
    button->update();
    return true;
}

void EmojiInput::release(Input& input)
{
    Taken& t = input.taken;
    disconnect(t.pressed);
    disconnect(t.clicked);
    disconnect(t.destroyed);
    if (QAbstractButton* button = t.button.data()) {
        button->removeEventFilter(this);
        button->setToolTip(t.toolTip);
        button->setAccessibleName(t.accessibleName);
        button->setAccessibleDescription(t.accessibleDescription);
        button->update(); // TeamSpeak paints it again (nothing else was changed)
    }
    t = Taken();
}

void EmojiInput::safetyNet(const QPointer<QTextEdit>& edit)
{
    // Posted (a zero timer is a posted event, freed with us at unload).
    QTimer::singleShot(0, this, [this, edit] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->inherits("EmoticonsDisplay") && w->isVisible())
                w->hide();
        }
        if (edit && !pickerOpen())
            openPicker(edit.data());
    });
}

void EmojiInput::sampleTone(Taken& t)
{
    QAbstractButton* button = t.button;
    if (!button)
        return;
    const QIcon icon = button->icon();
    t.iconKey        = icon.cacheKey();
    const QSize size = button->iconSize().isValid() ? button->iconSize() : QSize(16, 16);
    const QImage image = icon.isNull() ? QImage() : icon.pixmap(size).toImage().convertToFormat(QImage::Format_ARGB32);
    double r = 0, g = 0, b = 0, weight = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int a = qAlpha(line[x]);
            if (a < 128)
                continue;
            r += qRed(line[x]);
            g += qGreen(line[x]);
            b += qBlue(line[x]);
            weight += 1;
        }
    }
    if (weight > 0)
        t.tone = QColor(qRound(r / weight), qRound(g / weight), qRound(b / weight));
    else
        t.tone = isDark(button) ? QColor(0xa9, 0xaa, 0xac) : QColor(0x6d, 0x6f, 0x78);
}

QPixmap EmojiInput::facePixmap(int face, int logicalSize, int drawSize, qreal dpr, const QColor& tone, bool dark, int mode)
{
    const QString key = QString::fromLatin1("%1/%2/%3/%4/%5/%6/%7").arg(face).arg(logicalSize).arg(drawSize).arg(dpr).arg(tone.rgb()).arg(dark ? 1 : 0).arg(mode);
    const auto    it  = m_pixmaps.constFind(key);
    if (it != m_pixmaps.constEnd())
        return *it;
    if (m_pixmaps.size() > 64)
        m_pixmaps.clear();
    QImage canvas(QSize(logicalSize, logicalSize) * dpr, QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);
    QImage picture = emoji::render(face, drawSize, dpr);
    if (mode == kIdle)
        picture = toned(picture, tone.isValid() ? tone : (dark ? QColor(0xa9, 0xaa, 0xac) : QColor(0x6d, 0x6f, 0x78)), kIdleAlpha);
    else if (mode == kDisabled)
        picture = greyed(picture, kDisabledAlpha);
    if (!picture.isNull()) {
        QPainter p(&canvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setClipRect(QRectF(0, 0, logicalSize, logicalSize)); // a larger face is cut at the icon's edge
        const qreal offset = (logicalSize - drawSize) / 2.0;
        p.drawImage(QRectF(offset, offset, drawSize, drawSize), picture);
    }
    const QPixmap pixmap = QPixmap::fromImage(canvas);
    m_pixmaps.insert(key, pixmap);
    return pixmap;
}

void EmojiInput::paintTaken(Input& input)
{
    Taken&           t      = input.taken;
    QAbstractButton* button = t.button;
    if (!button)
        return;
    if (button->icon().cacheKey() != t.iconKey)
        sampleTone(t); // TeamSpeak gave it another icon (a skin switch)
    const bool  open    = pickerOpen() && m_last == input.edit;
    const bool  sunken  = t.held && t.inside;
    const bool  enabled = button->isEnabled();
    const bool  hover   = enabled && button->underMouse();
    const qreal dpr     = button->devicePixelRatioF();
    const bool  dark    = isDark(button);
    QSize       iconSize = button->iconSize();
    if (!iconSize.isValid() || iconSize.isEmpty())
        iconSize = QSize(16, 16);
    const int side = qMin(iconSize.width(), iconSize.height());

    QPixmap face;
    if (!enabled)
        face = facePixmap(smileFace(), side, side, dpr, t.tone, dark, kDisabled);
    else if (sunken)
        face = facePixmap(t.face, side, qMax(8, side - 2), dpr, t.tone, dark, kColour);
    else if (hover || open)
        face = facePixmap(t.face, side, side + 2, dpr, t.tone, dark, kColour);
    else
        face = facePixmap(smileFace(), side, side, dpr, t.tone, dark, kIdle);

    QStylePainter p(button);
    if (auto* tool = qobject_cast<QToolButton*>(button)) {
        QStyleOptionToolButton opt;
        ToolButtonAccess::init(tool, &opt);
        opt.icon     = iconOf(face);
        opt.iconSize = QSize(side, side);
        opt.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_On | QStyle::State_HasFocus);
        if (hover)
            opt.state |= QStyle::State_MouseOver;
        if (sunken) {
            opt.state |= QStyle::State_Sunken;
            opt.state &= ~QStyle::State_Raised;
            opt.activeSubControls |= QStyle::SC_ToolButton;
        }
        if (open) {
            opt.state |= QStyle::State_On; // the skin's :checked look while our picker is open
            opt.state &= ~QStyle::State_Raised;
        }
        if (button->hasFocus())
            opt.state |= QStyle::State_HasFocus;
        p.drawComplexControl(QStyle::CC_ToolButton, opt);
    } else if (auto* push = qobject_cast<QPushButton*>(button)) {
        QStyleOptionButton opt;
        PushButtonAccess::init(push, &opt);
        opt.icon     = iconOf(face);
        opt.iconSize = QSize(side, side);
        opt.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_On | QStyle::State_HasFocus);
        if (hover)
            opt.state |= QStyle::State_MouseOver;
        if (sunken) {
            opt.state |= QStyle::State_Sunken;
            opt.state &= ~QStyle::State_Raised;
        }
        if (open) {
            opt.state |= QStyle::State_On;
            opt.state &= ~QStyle::State_Raised;
        }
        if (button->hasFocus())
            opt.state |= QStyle::State_HasFocus;
        p.drawControl(QStyle::CE_PushButton, opt);
    }
}

void EmojiInput::activate(Input& input)
{
    if (!input.edit)
        return;
    if (pickerOpen()) {
        m_picker->close(); // a second click closes it
        return;
    }
    openPicker(input.edit.data());
}

bool EmojiInput::filterTaken(Input& input, QEvent* event)
{
    Taken&           t      = input.taken;
    QAbstractButton* button = t.button;
    switch (event->type()) {
    case QEvent::Paint:
        paintTaken(input);
        return true;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        // TeamSpeak never sees it: neither pressed() nor clicked() fires, no menu or popup can open.
        if (button->isEnabled()) {
            t.held   = true;
            t.inside = true;
            button->update();
        }
        return true;
    }
    case QEvent::MouseMove: {
        if (!t.held)
            return false;
        auto*      me     = static_cast<QMouseEvent*>(event);
        const bool inside = button->rect().contains(me->pos());
        if (inside != t.inside) {
            t.inside = inside;
            button->update();
        }
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        const bool was = t.held;
        t.held         = false;
        button->update();
        if (was && button->rect().contains(me->pos()))
            activate(input);
        return true;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto*     ke  = static_cast<QKeyEvent*>(event);
        const int key = ke->key();
        if (key != Qt::Key_Space && key != Qt::Key_Return && key != Qt::Key_Enter)
            return false;
        if (ke->isAutoRepeat())
            return true;
        if (event->type() == QEvent::KeyPress) {
            t.held   = true;
            t.inside = true;
            button->update();
        } else if (t.held) {
            t.held = false;
            button->update();
            activate(input);
        }
        return true;
    }
    case QEvent::Enter:
    case QEvent::HoverEnter:
        if (event->type() == QEvent::Enter)
            t.face = randomFace();
        button->update();
        return false;
    case QEvent::Leave:
    case QEvent::HoverLeave:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::EnabledChange:
        button->update();
        return false;
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
        sampleTone(t);
        button->update();
        return false;
    case QEvent::Hide:
    case QEvent::Show:
        // TeamSpeak hid it (its emoticon option) or showed it again: the fallback follows. A hidden
        // ancestor (the chat area going away) leaves it as it is.
        layout(input);
        return false;
    default:
        return false;
    }
}

// ---- the fallback inside the input -----------------------------------------------------------------------

void EmojiInput::showFallback(Input& input)
{
    QTextEdit* edit = input.edit;
    if (!edit)
        return;
    // 2.2.1 mic: slot 0 (the right edge) is the microphone's; the fallback goes left of it, in slot 1.
    reserveSlots(input, kMicSlots + 1);
    if (!input.button) {
        auto* button = new EmojiButton(edit);
        connect(button, &QAbstractButton::clicked, this, [this, guard = QPointer<QTextEdit>(edit)] {
            if (guard)
                openPicker(guard.data());
        });
        input.button = button;
    }
    // In the margin, right of the text: centred on a one-line input, at the bottom of a taller one.
    static_cast<EmojiButton*>(input.button.data())->place(slotRect(input, input.slotCount - 1));
    input.button->show();
    input.button->raise();
}

void EmojiInput::hideFallback(Input& input)
{
    if (input.button)
        delete input.button.data();
    // 2.2.1 mic: the microphone keeps its slot; only detach() gives the input its whole width back.
    reserveSlots(input, kMicSlots);
}

// ---- the input's right-edge slots ---------------------------------------------------------------------

void EmojiInput::reserveSlots(Input& input, int count)
{
    QTextEdit* edit = input.edit;
    if (count <= 0 || !edit) {
        if (input.ours && edit)
            ScrollAreaAccess::set(edit, input.original);
        input.ours      = false;
        input.slotCount = 0;
        return;
    }
    if (!input.ours) {
        input.original = ScrollAreaAccess::get(edit);
        input.ours     = true;
    }
    input.slotCount = count;
    const QMargins mine(input.original.left(), input.original.top(), input.original.right() + count * kMargin, input.original.bottom());
    if (ScrollAreaAccess::get(edit) != mine)
        ScrollAreaAccess::set(edit, mine);
}

QRect EmojiInput::slotRect(const Input& input, int index) const
{
    if (!input.edit || index < 0 || index >= input.slotCount)
        return {};
    // The strip right of the viewport (the margin reserveSlots made); slot 0 at the input's right edge.
    const QRect area = input.edit->viewport()->geometry();
    return QRect(area.right() + 1 + (input.slotCount - 1 - index) * kMargin, area.top(), kMargin, area.height());
}

void EmojiInput::layout(Input& input)
{
    QTextEdit* edit = input.edit;
    if (!edit)
        return;
    if (!Settings::instance().emojiButton) {
        // TeamSpeak's own button and popup as they are; Ctrl+E still opens ours.
        release(input);
        hideFallback(input);
    } else {
        QAbstractButton* button = input.taken.button ? input.taken.button.data() : findTeamSpeakButton(edit);
        if (button && supported(button))
            takeOver(input, button); // watched even while hidden: it may be shown again
        else if (input.taken.button)
            release(input);
        if (usable(input.taken.button))
            hideFallback(input);
        else
            showFallback(input);
    }
    // 2.2.1 mic: the microphone in slot 0, the input's right edge, on every path (micbutton.h). The slots
    // are kMicSlots + (the fallback shown ? 1 : 0); showFallback / hideFallback reserved them.
    MicButton::placeIn(edit, slotRect(input, 0));
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
    // 2.2.1: focused first (the chat had it: TeamSpeak's placeholder is in an empty input), then inserted.
    m_text->insert(target, emoji::text(id));
}

void EmojiInput::openPicker(QTextEdit* edit)
{
    if (!edit)
        return;
    if (m_picker && m_picker->isVisible()) {
        m_picker->close(); // the button again: closes it
        return;
    }
    if (m_picker)
        m_picker->deleteLater(); // closed and about to go: a new one now
    m_last          = edit;
    const bool dark = isDark(edit);
    auto*      picker = new EmojiPicker(EmojiPicker::Mode::Insert, dark, edit->palette().color(QPalette::Base), edit);
    m_picker          = picker;
    QPointer<QTextEdit> guard(edit);
    // 2.2.1: the picker has the focus, so an empty input holds TeamSpeak's placeholder: ChatInputs gives
    // the input the focus first (and the keyboard back to the picker when it stays open for more).
    connect(picker, &EmojiPicker::picked, this, [this, guard](int id, bool keepOpen) {
        if (guard)
            m_text->insert(guard.data(), emoji::text(id), keepOpen);
    });
    // Back to the input. Not as a popup's focus: TeamSpeak takes its placeholder out on an ordinary
    // focus-in, and text still waiting goes in after it.
    connect(picker, &EmojiPicker::closed, this, [this, guard] {
        if (guard)
            guard->setFocus(Qt::OtherFocusReason);
        // The button loses its "open" look (QPointer: the picker deletes itself after this).
        for (const Input& input : qAsConst(m_inputs)) {
            if (input.taken.button)
                input.taken.button->update();
        }
    });
    QRect anchor;
    for (const Input& input : qAsConst(m_inputs)) {
        if (input.edit != edit)
            continue;
        if (input.taken.button && input.taken.button->isVisible())
            anchor = QRect(input.taken.button->mapToGlobal(QPoint(0, 0)), input.taken.button->size());
        else if (input.button && input.button->isVisible())
            anchor = QRect(input.button->mapToGlobal(QPoint(0, 0)), input.button->size());
    }
    picker->setOpener(anchor); // the button again closes it (a click in the text just closes it and goes on)
    if (anchor.isNull())
        anchor = QRect(edit->mapToGlobal(QPoint(0, 0)), edit->size());
    picker->openAt(anchor); // above the button, right-aligned to it
    for (const Input& input : qAsConst(m_inputs)) {
        if (input.taken.button)
            input.taken.button->update(); // the "open" look
    }
}

bool EmojiInput::eventFilter(QObject* watched, QEvent* event)
{
    if (Input* owner = inputForButton(watched))
        return filterTaken(*owner, event);
    Input* input = inputFor(watched);
    if (!input) {
        // The viewport: laid out anew (a taller or shorter input, other margins).
        for (Input& candidate : m_inputs) {
            if (candidate.edit && candidate.edit->viewport() == watched && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
                if (candidate.ours)
                    layout(candidate);
                break;
            }
        }
        return false;
    }
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
    QObject::connect(action, &QAction::triggered, action, [guard, action] {
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
        // The field's button for this action: a second click on it closes the picker.
        for (QWidget* w : action->associatedWidgets()) {
            if (w != guard.data() && w->parentWidget() == guard.data() && w->isVisible())
                picker->setOpener(QRect(w->mapToGlobal(QPoint(0, 0)), w->size()));
        }
        // Below the field (the send window is in the middle of the screen).
        picker->openAt(QRect(guard->mapToGlobal(QPoint(0, 0)), guard->size()), true);
    });
}
