#include "emojipicker.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTabBar>
#include <QToolTip>
#include <QWheelEvent>

#include <climits>
#include <cmath>
#include <functional>

#include "emojidata.h"
#include "emojiprefs.h"
#include "emojirender.h"
#include "giflibrary.h" // 2.5 GIFs
#include "gifpanel.h"
#include "i18n.h"
#include "pickerstyle.h"
#include "uiutil.h"

namespace {

constexpr int kPad       = 8;
constexpr int kCell      = 40;
constexpr int kIcon      = 32;
constexpr int kColumns   = 9;
constexpr int kHeading   = 28;
constexpr int kSearch    = 32;
constexpr int kTabs      = 36;
constexpr int kTabIcon   = 22;
constexpr int kFooter    = 52;
constexpr int kBar       = 8;   // scroll bar
constexpr int kGridH     = 292; // 7 rows and a heading
constexpr int kTabCount  = 1 + emoji::kGroupCount; // recently used + the groups
constexpr int kMaxResults = 270;
// 2.5 GIFs: the switch row (Emoji | GIFs) on top moves everything of the emoji page down by kSwitchShift.
constexpr int kSwitchH     = 32;
constexpr int kSwitchShift = 42;
constexpr int kMore        = 28;

// The palette (2.5: shared with the GIFs tab, pickerstyle.h).
using pickerstyle::Colors;
using pickerstyle::colorsFor;

QFont scaled(const QFont& base, qreal px, int weight)
{
    QFont f = base;
    f.setPixelSize(qMax(8, qRound(px)));
    f.setWeight(weight);
    return f;
}

// A clock in the stroke style of the plugin's line icons (the "Recently used" tab).
void drawClock(QPainter& p, const QRectF& box, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, box.width() / 11.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const QRectF face = box.adjusted(box.width() * 0.1, box.height() * 0.1, -box.width() * 0.1, -box.height() * 0.1);
    p.drawEllipse(face);
    const QPointF c = face.center();
    p.drawLine(c, QPointF(c.x(), face.top() + face.height() * 0.24));
    p.drawLine(c, QPointF(c.x() + face.width() * 0.2, c.y() + face.height() * 0.12));
    p.restore();
}

// A magnifier for the search field.
void drawMagnifier(QPainter& p, const QRectF& box, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    const qreal r = box.width() * 0.32;
    const QPointF c(box.left() + box.width() * 0.42, box.top() + box.height() * 0.42);
    p.drawEllipse(c, r, r);
    p.drawLine(c + QPointF(r * 0.72, r * 0.72), QPointF(box.right() - box.width() * 0.12, box.bottom() - box.height() * 0.12));
    p.restore();
}

QImage greyed(const QImage& source)
{
    QImage out = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < out.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int g = qGray(line[x]);
            line[x]     = qRgba(g, g, g, qAlpha(line[x]));
        }
    }
    out.setDevicePixelRatio(source.devicePixelRatio());
    return out;
}

// ":joy:" for the footer: the first search word, or none.
QString shortcode(int id)
{
    const QString words = emoji::keywords(id);
    const QString first = words.section(QLatin1Char(' '), 0, 0);
    if (first.isEmpty() || first.startsWith(QLatin1Char('+')) || first.startsWith(QLatin1Char('-')))
        return {};
    return QLatin1Char(':') + first + QLatin1Char(':');
}

QString capitalized(QString text)
{
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

// Ctrl+E, the key that opened it, closes it again (the key, not the letter: other keyboard layouts too).
bool isPickerShortcut(const QKeyEvent* ke)
{
    const bool ctrl = (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier)) == Qt::ControlModifier;
    return ctrl && (ke->key() == Qt::Key_E || ke->nativeVirtualKey() == 'E');
}

// 2.5 GIFs: Ctrl+Tab / Ctrl+Shift+Tab switch the page (with a GIF host), guarded like Ctrl+E: TeamSpeak's window behind
// never sees them (its tab-cycling shortcut would switch the chat behind the popup).
bool isPageSwitchKey(const QKeyEvent* ke)
{
    const Qt::KeyboardModifiers mods = ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    return (mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::ShiftModifier)) && (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab);
}

} // namespace

// ============================================================================================
// The grid: sections of emoji, scrolled by itself (no QScrollArea: TeamSpeak's style sheets stay out)
// ============================================================================================

class EmojiGrid : public QWidget
{
  public:
    struct Section {
        QString      title;
        int          tab = 0; // 0: recently used / search results, 1..: groups
        QVector<int> ids;
        int          top = 0; // content coordinates
    };

    std::function<void(int item)>            onHover;
    std::function<void(int item, bool keep)> onActivate;
    std::function<void()>                    onScrolled;

    EmojiGrid(const Colors* colors, QWidget* parent)
        : QWidget(parent)
        , m_colors(colors)
    {
        setObjectName(QString::fromLatin1("tsmediaEmojiGrid"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAccessibleName(i18n::t("Emoji"));
        setAccessibleDescription(i18n::t("Arrow keys move, Page Up and Page Down jump a group, Enter picks."));
        if (emoji::ImageNotifier* n = emoji::notifier())
            connect(n, &emoji::ImageNotifier::imagesReady, this, [this] { update(); });
    }

    void setSections(const QVector<Section>& sections, const QString& empty)
    {
        m_sections = sections;
        m_empty    = empty;
        m_items.clear();
        m_sectionOf.clear();
        m_rects.clear();
        int y = 0;
        for (int s = 0; s < m_sections.size(); ++s) {
            Section& section = m_sections[s];
            section.top      = y;
            y += kHeading;
            for (int i = 0; i < section.ids.size(); ++i) {
                m_items.append(section.ids.at(i));
                m_sectionOf.append(s);
                m_rects.append(QRect((i % kColumns) * kCell, y + (i / kColumns) * kCell, kCell, kCell));
            }
            y += ((section.ids.size() + kColumns - 1) / kColumns) * kCell;
        }
        m_contentHeight = y + 4;
        m_hover         = -1;
        m_pressed       = -1;
        m_focus         = m_items.isEmpty() ? -1 : 0;
        setScroll(0);
        update();
    }

    void setMarked(const QSet<int>& marked)
    {
        m_marked = marked;
        update();
    }

    int        itemCount() const { return m_items.size(); }
    int        idAt(int item) const { return m_items.value(item, -1); }
    int        focus() const { return m_focus; }
    int        hover() const { return m_hover; }
    bool       keyboardUsed() const { return m_keyboard; }
    int        sectionCount() const { return m_sections.size(); }
    const Section& section(int s) const { return m_sections.at(s); }

    // The section at the top of the view (the active tab).
    int topSection() const
    {
        int current = 0;
        for (int s = 0; s < m_sections.size(); ++s) {
            if (m_sections.at(s).top <= m_scroll + 4)
                current = s;
        }
        return current;
    }

    void scrollToSection(int s)
    {
        if (s >= 0 && s < m_sections.size())
            setScroll(m_sections.at(s).top);
    }

    void setFocusItem(int item, bool fromKeyboard)
    {
        if (m_items.isEmpty())
            return;
        m_focus    = qBound(0, item, m_items.size() - 1);
        m_keyboard = m_keyboard || fromKeyboard;
        ensureVisible(m_focus);
        if (onHover)
            onHover(m_focus);
        // Screen readers: the grid is one widget, so its name says which emoji has the focus (Qt
        // announces the change).
        if (fromKeyboard) {
            const QString name = capitalized(emoji::name(m_items.at(m_focus)));
            setAccessibleName(name.isEmpty() ? i18n::t("Emoji") : name);
        }
        update();
    }

    void prefetchAround()
    {
        // The rest of what is on screen first (urgent, as painted), then the next screens ahead.
        QVector<int> ahead;
        for (int i = 0; i < m_items.size() && ahead.size() < 200; ++i) {
            if (m_rects.at(i).top() >= m_scroll + height())
                ahead.append(m_items.at(i));
        }
        emoji::prefetch(ahead, kIcon, devicePixelRatioF());
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_colors->background);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal dpr = devicePixelRatioF();
        if (m_items.isEmpty()) {
            p.setPen(m_colors->muted);
            p.setFont(scaled(font(), 13, QFont::Normal));
            p.drawText(rect().adjusted(24, 0, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_empty);
            return;
        }
        const QFont headingFont = scaled(font(), 11.5, QFont::DemiBold);
        p.setFont(headingFont);
        for (const Section& s : qAsConst(m_sections)) {
            const int y = s.top - m_scroll;
            if (y + kHeading < 0 || y > height())
                continue;
            p.setPen(m_colors->heading);
            p.drawText(QRect(6, y, width() - 12, kHeading), Qt::AlignLeft | Qt::AlignVCenter, s.title.toUpper());
        }
        for (int i = 0; i < m_items.size(); ++i) {
            const QRect r = m_rects.at(i).translated(0, -m_scroll);
            if (r.bottom() < 0 || r.top() > height())
                continue;
            const int  id      = m_items.at(i);
            const bool focused = i == m_focus && hasFocus() && m_keyboard;
            const QRectF cell  = QRectF(r).adjusted(2, 2, -2, -2);
            if (m_marked.contains(id)) {
                p.setPen(Qt::NoPen);
                p.setBrush(m_colors->marked);
                p.drawRoundedRect(cell, 6, 6);
            }
            if (i == m_pressed || i == m_hover || focused) {
                p.setPen(Qt::NoPen);
                p.setBrush(i == m_pressed ? m_colors->pressed : m_colors->hover);
                p.drawRoundedRect(cell, 6, 6);
            }
            if (focused) {
                p.setPen(QPen(m_colors->ring, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(cell.adjusted(1, 1, -1, -1), 5, 5);
            }
            const QRectF box(r.left() + (kCell - kIcon) / 2.0, r.top() + (kCell - kIcon) / 2.0, kIcon, kIcon);
            const QImage image = emoji::requestImage(id, kIcon, dpr);
            if (image.isNull()) {
                p.setPen(Qt::NoPen);
                p.setBrush(m_colors->dot);
                p.drawEllipse(box.adjusted(5, 5, -5, -5)); // until the worker has drawn it
            } else {
                p.drawImage(box, image);
            }
        }
        // The scroll bar (only when there is more than fits).
        if (m_contentHeight > height()) {
            const QRectF bar = thumbRect();
            p.setPen(Qt::NoPen);
            p.setBrush(m_dragging || m_overBar ? m_colors->muted : m_colors->thumb);
            p.drawRoundedRect(bar, bar.width() / 2, bar.width() / 2);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging) {
            const qreal track = height() - thumbRect().height();
            if (track > 0)
                setScroll(m_dragScroll + qRound((event->pos().y() - m_dragY) * (m_contentHeight - height()) / track));
            return;
        }
        const bool over = m_contentHeight > height() && event->pos().x() >= width() - kBar - 2;
        if (over != m_overBar) {
            m_overBar = over;
            update();
        }
        setHover(over ? -1 : itemAt(event->pos()));
    }

    void leaveEvent(QEvent*) override
    {
        m_overBar = false;
        setHover(-1);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_contentHeight > height() && event->pos().x() >= width() - kBar - 2) {
            const QRectF thumb = thumbRect();
            if (!thumb.contains(event->pos())) // a click on the track pages there
                setScroll(m_scroll + (event->pos().y() < thumb.top() ? -height() : height()) + kCell);
            m_dragging   = true;
            m_dragY      = event->pos().y();
            m_dragScroll = m_scroll;
            return;
        }
        m_pressed = itemAt(event->pos());
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_dragging) {
            m_dragging = false;
            update();
            return;
        }
        const int pressed = m_pressed;
        m_pressed         = -1;
        update();
        if (event->button() == Qt::LeftButton && pressed >= 0 && pressed == itemAt(event->pos()) && onActivate)
            onActivate(pressed, event->modifiers() & Qt::ShiftModifier);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        const QPoint pixels = event->pixelDelta();
        const int    delta  = !pixels.isNull() ? pixels.y() : event->angleDelta().y() * kCell * 2 / 120;
        setScroll(m_scroll - delta);
        setHover(itemAt(event->position().toPoint()));
        event->accept();
    }

    void resizeEvent(QResizeEvent*) override { setScroll(m_scroll); }
    void focusInEvent(QFocusEvent*) override { update(); }
    void focusOutEvent(QFocusEvent*) override { update(); }

  private:
    int itemAt(const QPoint& pos) const
    {
        const QPoint content(pos.x(), pos.y() + m_scroll);
        for (int i = 0; i < m_rects.size(); ++i) {
            if (m_rects.at(i).contains(content))
                return i;
        }
        return -1;
    }

    void setHover(int item)
    {
        if (item == m_hover)
            return;
        m_hover = item;
        if (onHover)
            onHover(item >= 0 ? item : (m_keyboard ? m_focus : -1));
        update();
    }

    QRectF thumbRect() const
    {
        const qreal visible = static_cast<qreal>(height()) / qMax(1, m_contentHeight);
        const qreal h       = qMax<qreal>(28, height() * visible);
        const int   range   = qMax(1, m_contentHeight - height());
        const qreal y       = (height() - h) * m_scroll / range;
        return QRectF(width() - kBar + 1, y + 2, kBar - 3, h - 4);
    }

    void ensureVisible(int item)
    {
        const QRect r = m_rects.value(item);
        int         s = m_scroll;
        if (r.top() - kHeading < s) {
            // The first row of a section brings its heading along.
            const int section = m_sectionOf.value(item);
            s                 = qMin(r.top(), m_sections.at(section).top + (r.top() - m_sections.at(section).top <= kHeading ? 0 : kHeading)) - 4;
            s                 = qMin(s, r.top() - 4);
        } else if (r.bottom() > s + height()) {
            s = r.bottom() - height() + 6;
        }
        setScroll(s);
    }

    void setScroll(int value)
    {
        m_scroll = qBound(0, value, qMax(0, m_contentHeight - height()));
        update();
        if (onScrolled)
            onScrolled();
    }

    friend class EmojiGridKeys;

    const Colors*    m_colors;
    QVector<Section> m_sections;
    QVector<int>     m_items;
    QVector<int>     m_sectionOf;
    QVector<QRect>   m_rects; // content coordinates
    QSet<int>        m_marked;
    QString          m_empty;
    int              m_contentHeight = 0;
    int              m_scroll        = 0;
    int              m_hover         = -1;
    int              m_pressed       = -1;
    int              m_focus         = -1;
    bool             m_keyboard      = false;
    bool             m_dragging      = false;
    bool             m_overBar       = false;
    int              m_dragY         = 0;
    int              m_dragScroll    = 0;
};

// ============================================================================================
// Skin tones: a button with the waving hand in the current tone, and the row to choose one
// ============================================================================================

class ToneButton : public QAbstractButton
{
  public:
    ToneButton(const Colors* colors, QWidget* parent)
        : QAbstractButton(parent)
        , m_colors(colors)
    {
        setFixedSize(kSearch, kSearch);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setAccessibleName(i18n::t("Skin tone"));
        setToolTip(i18n::t("Skin tone"));
    }

    void setTone(int tone)
    {
        m_tone = tone;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isDown() || underMouse() || hasFocus()) {
            p.setPen(hasFocus() ? QPen(m_colors->ring, 2.0) : Qt::NoPen);
            p.setBrush(isDown() ? m_colors->pressed : m_colors->hover);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
        }
        const int    wave = emoji::find(QString::fromUtf8("\xF0\x9F\x91\x8B"));
        const QImage hand = emoji::render(emoji::withTone(wave, m_tone), 22, devicePixelRatioF());
        p.drawImage(QRectF((width() - 22) / 2.0, (height() - 22) / 2.0, 22, 22), hand);
    }

  private:
    const Colors* m_colors;
    int           m_tone = 0;
};

class TonePopup : public QWidget
{
  public:
    std::function<void(int tone)> onPick;

    TonePopup(const Colors* colors, QWidget* parent)
        : QWidget(parent)
        , m_colors(colors)
    {
        setFixedSize(6 * 34 + 8, 42);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAccessibleName(i18n::t("Skin tone"));
        hide();
    }

    void open(int current)
    {
        m_focus = current;
        show();
        raise();
        setFocus(Qt::PopupFocusReason);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(m_colors->border, 1.0));
        p.setBrush(m_colors->background);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        const int wave = emoji::find(QString::fromUtf8("\xF0\x9F\x91\x8B"));
        for (int t = 0; t <= emoji::kToneCount; ++t) {
            const QRectF cell = cellRect(t);
            if (t == m_focus || t == m_hover) {
                p.setPen(t == m_focus && hasFocus() ? QPen(m_colors->ring, 2.0) : Qt::NoPen);
                p.setBrush(m_colors->hover);
                p.drawRoundedRect(cell.adjusted(1, 1, -1, -1), 6, 6);
            }
            p.drawImage(QRectF(cell.center().x() - 12, cell.center().y() - 12, 24, 24), emoji::render(emoji::withTone(wave, t), 24, devicePixelRatioF()));
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const int at = toneAt(event->pos());
        if (at != m_hover) {
            m_hover = at;
            if (at >= 0)
                QToolTip::showText(event->globalPos(), toneLabel(at), this, cellRect(at).toRect());
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const int at = toneAt(event->pos());
        if (at >= 0)
            pick(at);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        switch (event->key()) {
        case Qt::Key_Left:
            m_focus = m_focus <= 0 ? emoji::kToneCount : m_focus - 1;
            update();
            return;
        case Qt::Key_Right:
            m_focus = m_focus >= emoji::kToneCount ? 0 : m_focus + 1;
            update();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            pick(m_focus);
            return;
        case Qt::Key_Escape:
            hide();
            if (parentWidget())
                parentWidget()->setFocus();
            return;
        default:
            QWidget::keyPressEvent(event);
        }
    }

    void focusOutEvent(QFocusEvent*) override { hide(); }

  private:
    static QString toneLabel(int t) { return t == 0 ? i18n::t("Default") : capitalized(emoji::toneName(t)); }
    QRectF         cellRect(int t) const { return QRectF(4 + t * 34, 4, 34, 34); }
    int            toneAt(const QPoint& pos) const
    {
        for (int t = 0; t <= emoji::kToneCount; ++t) {
            if (cellRect(t).contains(pos))
                return t;
        }
        return -1;
    }
    void pick(int t)
    {
        hide();
        if (onPick)
            onPick(t);
    }

    const Colors* m_colors;
    int           m_focus = 0;
    int           m_hover = -1;
};

// ============================================================================================
// The picker
// ============================================================================================

class EmojiPickerPrivate
{
  public:
    EmojiPicker*  q;
    EmojiPicker::Mode mode;
    bool          dark;
    Colors        colors;
    QLineEdit*    search = nullptr;
    ToneButton*   toneButton = nullptr;
    TonePopup*    tones = nullptr;
    EmojiGrid*    grid = nullptr;
    int           tone = 0;
    int           hoverTab = -1;
    int           footerId = -1;
    QVector<int>  tabSection; // tab -> section index in the grid (-1: none)
    QVector<bool> tabHasItems = QVector<bool>(kTabCount, true); // from the last layout without a search
    QSet<int>     marked;
    QRect         opener; // the button it was opened with (global): a press there closes it for good
    // 2.5 GIFs
    int                top    = 0; // kSwitchShift with a GIF host: every absolute place of the emoji page goes through it
    bool               hosted = false;
    gifs::Host*        host   = nullptr; // owned
    gifs::SendState*   opened = nullptr; // owned: the state when it opened (its fingerprint)
    EmojiPicker::Page  page   = EmojiPicker::Page::Emoji;
    QWidget*           switcher = nullptr;
    QAbstractButton*   more     = nullptr;
    GifPanel*          gifs     = nullptr;
    QObject*           pickerKeys = nullptr;

    QRect tabsRect() const { return QRect(kPad, top + kPad + kSearch + 6, kColumns * kCell, kTabs); }
    QRect tabRect(int tab) const
    {
        const QRect bar = tabsRect();
        const int   w   = bar.width() / kTabCount;
        return QRect(bar.left() + tab * w, bar.top(), w, bar.height());
    }
    QRect footerRect() const { return QRect(0, q->height() - kFooter, q->width(), kFooter); }

    int tabAt(const QPoint& pos) const
    {
        if (page == EmojiPicker::Page::Gifs)
            return -1; // the GIFs page owns its area
        for (int t = 0; t < kTabCount; ++t) {
            if (tabRect(t).contains(pos))
                return t;
        }
        return -1;
    }

    int groupIcon(int tab) const
    {
        static const char* const icons[emoji::kGroupCount] = {
            "\xF0\x9F\x98\x80", // 😀
            "\xF0\x9F\x90\xBB", // 🐻
            "\xF0\x9F\x8D\x94", // 🍔
            "\xE2\x9A\xBD",     // ⚽
            "\xF0\x9F\x9A\x97", // 🚗
            "\xF0\x9F\x92\xA1", // 💡
            "\xF0\x9F\x94\xA3", // 🔣
            "\xF0\x9F\x8F\x81", // 🏁
        };
        return tab >= 1 && tab <= emoji::kGroupCount ? emoji::find(QString::fromUtf8(icons[tab - 1])) : -1;
    }

    QString tabName(int tab) const
    {
        return tab == 0 ? i18n::t("Recently used") : emoji::groupName(static_cast<emoji::Group>(tab - 1));
    }

    // With the chosen skin tone; the base when this PC's font has no picture for that variant.
    int display(int id) const
    {
        if (tone <= 0)
            return id;
        const int variant = emoji::withTone(id, tone);
        return variant != id && emoji::supported(variant) ? variant : id;
    }

    void rebuild()
    {
        QVector<EmojiGrid::Section> sections;
        tabSection = QVector<int>(kTabCount, -1);
        const QString query = search->text().trimmed();
        QString       empty;
        if (!query.isEmpty()) {
            EmojiGrid::Section results;
            results.title = i18n::t("Search results");
            for (int id : emoji::search(query, kMaxResults)) {
                if (emoji::supported(id))
                    results.ids.append(display(id));
            }
            if (!results.ids.isEmpty())
                sections.append(results);
            empty = i18n::t("No emoji match “%1”. Try a word like “smile”, “heart” or “cat”.").arg(query.left(40));
        } else {
            EmojiGrid::Section recent;
            recent.title = tabName(0);
            for (int id : emoji::prefs::recent()) {
                if (emoji::supported(id))
                    recent.ids.append(id); // as used, tone included
            }
            if (!recent.ids.isEmpty()) {
                tabSection[0] = sections.size();
                sections.append(recent);
            }
            for (int g = 0; g < emoji::kGroupCount; ++g) {
                EmojiGrid::Section section;
                section.title = emoji::groupName(static_cast<emoji::Group>(g));
                section.tab   = g + 1;
                for (int id : emoji::members(static_cast<emoji::Group>(g))) {
                    if (emoji::supported(id))
                        section.ids.append(display(id));
                }
                if (section.ids.isEmpty())
                    continue;
                tabSection[g + 1] = sections.size();
                sections.append(section);
            }
            for (int t = 0; t < kTabCount; ++t)
                tabHasItems[t] = tabSection.at(t) >= 0;
        }
        grid->setSections(sections, empty);
        grid->setMarked(marked);
        footerId = grid->idAt(0);
        q->update();
    }

    int activeTab() const
    {
        if (!search->text().trimmed().isEmpty())
            return -1;
        const int topSection = grid->topSection();
        for (int t = 0; t < kTabCount; ++t) {
            if (tabSection.value(t) == topSection)
                return t;
        }
        return -1;
    }

    // Every absolute place of the emoji page, through top (design_gifs 5.1).
    void place()
    {
        search->setGeometry(kPad, top + kPad, kColumns * kCell - kSearch - 6, kSearch);
        toneButton->move(kPad + kColumns * kCell - kSearch, top + kPad);
        tones->move(q->width() - kPad - tones->width(), top + kPad + kSearch + 4);
        const int gridTop = tabsRect().bottom() + 5;
        grid->setGeometry(kPad, gridTop, kColumns * kCell + kBar, q->height() - kFooter - gridTop - 4);
    }

    void togglePage()
    {
        q->showPage(page == EmojiPicker::Page::Emoji ? EmojiPicker::Page::Gifs : EmojiPicker::Page::Emoji);
        focusPage();
    }

    void focusPage()
    {
        if (page == EmojiPicker::Page::Gifs && gifs)
            gifs->focusGrid(Qt::OtherFocusReason);
        else
            search->setFocus(Qt::OtherFocusReason);
    }

    void activate(int item, bool keep)
    {
        const int id = grid->idAt(item);
        if (!emoji::isValid(id))
            return;
        emoji::prefs::noteUsed(id);
        emit q->picked(id, keep);
        if (!keep)
            q->close();
    }

    void paintTabs(QPainter& p)
    {
        const int   active = activeTab();
        const qreal dpr    = q->devicePixelRatioF();
        for (int t = 0; t < kTabCount; ++t) {
            const QRect r      = tabRect(t);
            const bool  on     = t == active;
            const bool  hover  = t == hoverTab;
            const bool  usable = tabHasItems.value(t);
            if (hover && usable) {
                p.setPen(Qt::NoPen);
                p.setBrush(colors.hover);
                p.drawRoundedRect(QRectF(r).adjusted(3, 2, -3, -4), 6, 6);
            }
            const QRectF icon(r.center().x() - kTabIcon / 2.0 + 0.5, r.top() + (r.height() - kTabIcon) / 2.0 - 1, kTabIcon, kTabIcon);
            p.setOpacity(usable ? 1.0 : 0.35);
            if (t == 0) {
                drawClock(p, icon.adjusted(1, 1, -1, -1), on || hover ? colors.text : colors.muted);
            } else {
                const QImage image = emoji::render(groupIcon(t), kTabIcon, dpr);
                if (on || hover) {
                    p.drawImage(icon, image);
                } else {
                    p.setOpacity(usable ? 0.75 : 0.3);
                    p.drawImage(icon, greyed(image));
                }
            }
            p.setOpacity(1.0);
            if (on) {
                p.setPen(Qt::NoPen);
                p.setBrush(colors.accent);
                p.drawRoundedRect(QRectF(r.center().x() - 9, r.bottom() - 2, 18, 3), 1.5, 1.5);
            }
        }
    }

    void paintFooter(QPainter& p)
    {
        const QRect r = footerRect();
        p.setPen(QPen(colors.divider, 1.0));
        p.drawLine(QPointF(0, r.top() + 0.5), QPointF(q->width(), r.top() + 0.5));
        if (!emoji::isValid(footerId)) {
            p.setPen(colors.muted);
            p.setFont(scaled(q->font(), 12.5, QFont::Normal));
            p.drawText(r.adjusted(16, 0, -16, 0), Qt::AlignLeft | Qt::AlignVCenter, i18n::t("Pick an emoji"));
            return;
        }
        const qreal dpr   = q->devicePixelRatioF();
        QImage      image = emoji::requestImage(footerId, 36, dpr);
        if (image.isNull())
            image = emoji::cachedImage(emoji::text(footerId), kIcon, dpr); // the grid's, until the larger one is drawn
        const QRectF box(14, r.top() + (kFooter - 36) / 2.0, 36, 36);
        if (!image.isNull())
            p.drawImage(box, image);
        const QFont nameFont = scaled(q->font(), 13.5, QFont::DemiBold);
        const QFont codeFont = scaled(q->font(), 12, QFont::Normal);
        const int   left     = 14 + 36 + 12;
        const int   width    = q->width() - left - 14;
        const QString code   = shortcode(footerId);
        QFontMetrics nm(nameFont);
        p.setFont(nameFont);
        p.setPen(colors.text);
        const int nameTop = code.isEmpty() ? r.top() + (kFooter - nm.height()) / 2 : r.top() + 8;
        p.drawText(QRect(left, nameTop, width, nm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   nm.elidedText(capitalized(emoji::name(footerId)), Qt::ElideRight, width));
        if (!code.isEmpty()) {
            p.setFont(codeFont);
            p.setPen(colors.muted);
            p.drawText(QRect(left, nameTop + nm.height() + 1, width, QFontMetrics(codeFont).height()), Qt::AlignLeft | Qt::AlignVCenter, code);
        }
    }
};

// The grid's keys: the picker's navigation rules, as an event filter on the grid.
class EmojiGridKeys : public QObject
{
  public:
    EmojiGridKeys(EmojiPickerPrivate* d, QObject* parent)
        : QObject(parent)
        , m_d(d)
    {
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride
            && (isPickerShortcut(static_cast<QKeyEvent*>(event)) || (m_d->hosted && isPageSwitchKey(static_cast<QKeyEvent*>(event))))) {
            event->accept(); // comes as a key press, not to a shortcut of the window behind
            return true;
        }
        if (event->type() != QEvent::KeyPress)
            return false;
        auto*      ke    = static_cast<QKeyEvent*>(event);
        if (isPickerShortcut(ke)) {
            m_d->q->close();
            return true;
        }
        if (m_d->hosted && isPageSwitchKey(ke)) {
            m_d->togglePage();
            return true;
        }
        EmojiGrid* grid  = m_d->grid;
        const int  focus = qMax(0, grid->focus());
        const int  count = grid->itemCount();
        if (count == 0)
            return false;
        auto rowStep = [&](int direction) {
            // The nearest row in that direction (sections have their own rows), and in it the item
            // closest to the same column.
            const QRect here   = grid->m_rects.value(focus);
            int         target = direction > 0 ? INT_MAX : INT_MIN;
            for (const QRect& r : qAsConst(grid->m_rects)) {
                if (direction > 0 && r.top() > here.top())
                    target = qMin(target, r.top());
                else if (direction < 0 && r.top() < here.top())
                    target = qMax(target, r.top());
            }
            int best = -1;
            for (int i = 0; i < count; ++i) {
                const QRect& r = grid->m_rects.at(i);
                if (r.top() == target && (best < 0 || qAbs(r.left() - here.left()) < qAbs(grid->m_rects.at(best).left() - here.left())))
                    best = i;
            }
            return best;
        };
        switch (ke->key()) {
        case Qt::Key_Left:
            grid->setFocusItem(focus - 1, true);
            return true;
        case Qt::Key_Right:
            grid->setFocusItem(focus + 1, true);
            return true;
        case Qt::Key_Down: {
            const int next = rowStep(1);
            if (next >= 0)
                grid->setFocusItem(next, true);
            return true;
        }
        case Qt::Key_Up: {
            const int previous = rowStep(-1);
            if (previous >= 0)
                grid->setFocusItem(previous, true);
            else
                m_d->search->setFocus(Qt::BacktabFocusReason); // from the first row back to the search
            return true;
        }
        case Qt::Key_Home:
            grid->setFocusItem(0, true);
            return true;
        case Qt::Key_End:
            grid->setFocusItem(count - 1, true);
            return true;
        case Qt::Key_PageDown:
        case Qt::Key_PageUp: {
            const int section = grid->m_sectionOf.value(focus) + (ke->key() == Qt::Key_PageDown ? 1 : -1);
            if (section >= 0 && section < grid->sectionCount()) {
                int first = 0;
                for (int s = 0; s < section; ++s)
                    first += grid->section(s).ids.size();
                grid->setFocusItem(first, true);
            }
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            m_d->activate(focus, ke->modifiers() & Qt::ShiftModifier);
            return true;
        case Qt::Key_Escape:
            // As in the search field: the search is cleared first, then it closes.
            if (!m_d->search->text().isEmpty()) {
                m_d->search->clear();
                m_d->search->setFocus(Qt::OtherFocusReason);
            } else {
                m_d->q->close();
            }
            return true;
        case Qt::Key_Backtab:
            m_d->search->setFocus(Qt::BacktabFocusReason);
            return true;
        default:
            break;
        }
        // Typing goes to the search.
        const QString text = ke->text();
        if (!text.isEmpty() && text.at(0).isPrint() && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            m_d->search->setFocus(Qt::OtherFocusReason);
            m_d->search->insert(text);
            return true;
        }
        if (ke->key() == Qt::Key_Backspace) {
            m_d->search->setFocus(Qt::OtherFocusReason);
            m_d->search->backspace();
            return true;
        }
        return false;
    }

  private:
    EmojiPickerPrivate* m_d;
};

// ============================================================================================
// 2.5 GIFs: the switch row, the ⋯ button, and the keys every new child guards
// ============================================================================================

// Ctrl+E closes the picker and Ctrl+Tab switches its page from the switch row, the ⋯ button, the GIF grid, the bar's
// buttons and the tone button; their ShortcutOverride is accepted, so TeamSpeak's window behind never sees them.
class PickerKeys : public QObject
{
  public:
    PickerKeys(EmojiPickerPrivate* d, QObject* parent)
        : QObject(parent)
        , m_d(d)
    {
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
            return false;
        auto*      ke     = static_cast<QKeyEvent*>(event);
        const bool picker = isPickerShortcut(ke);
        const bool page   = m_d->hosted && isPageSwitchKey(ke);
        if (!picker && !page)
            return false;
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (picker)
            m_d->q->close();
        else
            m_d->togglePage();
        return true;
    }

  private:
    EmojiPickerPrivate* m_d;
};

// "Emoji" | "GIFs": two text tabs (design_gifs 5.2). One focus stop; Left / Right move between the tabs, Enter or Space
// shows the focused one, Down goes into the page.
class PickerSwitch : public QWidget
{
  public:
    PickerSwitch(EmojiPickerPrivate* d, QWidget* parent)
        : QWidget(parent)
        , m_d(d)
    {
        setObjectName(QString::fromLatin1("tsmediaPickerSwitch"));
        setMouseTracking(true);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        updateAccessible();
    }

    QRect tabRect(int tab) const
    {
        const QFontMetrics fm(labelFont());
        const int          emojiW = fm.horizontalAdvance(label(0)) + 24;
        return tab == 0 ? QRect(0, 0, emojiW, kSwitchH) : QRect(emojiW + 4, 0, fm.horizontalAdvance(label(1)) + 24, kSwitchH);
    }

    void updateAccessible()
    {
        const int active = m_d->page == EmojiPicker::Page::Gifs ? 1 : 0;
        const int shown  = hasFocus() ? m_focusTab : active;
        setAccessibleName(label(shown) + (shown == active ? i18n::t(", selected") : QString()));
        setAccessibleDescription(i18n::t("Emoji and GIFs pages. Ctrl+Tab switches."));
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const pickerstyle::Colors& c      = m_d->colors;
        const int                  active = m_d->page == EmojiPicker::Page::Gifs ? 1 : 0;
        const QFont                f      = labelFont();
        const QFontMetrics         fm(f);
        p.setFont(f);
        for (int t = 0; t < 2; ++t) {
            const QRect r = tabRect(t);
            if (t == m_hover) {
                p.setPen(Qt::NoPen);
                p.setBrush(c.hover);
                p.drawRoundedRect(QRectF(r.left(), 2, r.width(), 28), 6, 6);
            }
            if (hasFocus() && t == m_focusTab) {
                p.setPen(QPen(c.ring, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(QRectF(r.left() + 1, 3, r.width() - 2, 26), 6, 6);
            }
            p.setPen(t == active ? c.text : c.muted);
            p.drawText(QRect(r.left(), 2, r.width(), 26), Qt::AlignCenter, label(t));
            if (t == active) {
                const int w = fm.horizontalAdvance(label(t));
                p.setPen(Qt::NoPen);
                p.setBrush(c.accent);
                p.drawRoundedRect(QRectF(r.center().x() - w / 2.0 + 0.5, 29, w, 3), 1.5, 1.5);
            }
        }
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto*     he  = static_cast<QHelpEvent*>(event);
            const int tab = tabAt(he->pos());
            if (tab >= 0)
                QToolTip::showText(he->globalPos(), tab == 0 ? i18n::t("Emoji (Ctrl+Tab)") : i18n::t("GIFs (Ctrl+Tab)"), this, tabRect(tab));
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const int tab = tabAt(event->pos());
        if (tab != m_hover) {
            m_hover = tab;
            update();
        }
    }

    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const int tab = tabAt(event->pos());
        if (event->button() == Qt::LeftButton && tab >= 0) {
            m_focusTab = tab;
            m_d->q->showPage(tab == 1 ? EmojiPicker::Page::Gifs : EmojiPicker::Page::Emoji);
            m_d->focusPage();
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
            m_focusTab = event->key() == Qt::Key_Left ? 0 : 1;
            updateAccessible();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            m_d->q->showPage(m_focusTab == 1 ? EmojiPicker::Page::Gifs : EmojiPicker::Page::Emoji);
            updateAccessible();
            return;
        case Qt::Key_Down:
            m_d->focusPage();
            return;
        case Qt::Key_Escape:
            m_d->q->close();
            return;
        default:
            QWidget::keyPressEvent(event);
        }
    }

    void focusInEvent(QFocusEvent*) override
    {
        m_focusTab = m_d->page == EmojiPicker::Page::Gifs ? 1 : 0;
        updateAccessible();
    }
    void focusOutEvent(QFocusEvent*) override { update(); }

  private:
    static QString label(int tab) { return tab == 0 ? i18n::t("Emoji") : i18n::t("GIFs"); }
    QFont          labelFont() const
    {
        QFont f = font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        return f;
    }
    int tabAt(const QPoint& pos) const
    {
        for (int t = 0; t < 2; ++t) {
            if (tabRect(t).contains(pos))
                return t;
        }
        return -1;
    }

    EmojiPickerPrivate* m_d;
    int                 m_hover    = -1;
    int                 m_focusTab = 0;
};

// The GIFs page's ⋯ button: its options menu.
class MoreButton : public QAbstractButton
{
  public:
    MoreButton(const pickerstyle::Colors* colors, QWidget* parent)
        : QAbstractButton(parent)
        , m_colors(colors)
    {
        setObjectName(QString::fromLatin1("tsmediaGifMore"));
        setFixedSize(kMore, kMore);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(i18n::t("More"));
        setAccessibleName(i18n::t("GIF options"));
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || isDown() || hasFocus()) {
            p.setPen(hasFocus() ? QPen(m_colors->ring, 2.0) : Qt::NoPen);
            p.setBrush(isDown() ? m_colors->pressed : m_colors->hover);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(m_colors->muted);
        const qreal cy = height() / 2.0;
        for (int i = -1; i <= 1; ++i)
            p.drawEllipse(QPointF(width() / 2.0 + i * 6.0, cy), 1.5, 1.5);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space || event->key() == Qt::Key_Down) {
            click();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

  private:
    const pickerstyle::Colors* m_colors;
};

EmojiPicker::EmojiPicker(Mode mode, bool dark, const QColor& base, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
    , d(new EmojiPickerPrivate{this, mode, dark, colorsFor(dark, base)})
{
    setObjectName(QString::fromLatin1("tsmediaEmojiPicker"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setLayoutDirection(Qt::LeftToRight);
    setMouseTracking(true);
    setAccessibleName(mode == Mode::React ? i18n::t("Add reaction") : i18n::t("Emoji"));
    setFixedSize(preferredSize());
    d->tone = emoji::prefs::tone();

    d->search = new QLineEdit(this);
    d->search->setObjectName(QString::fromLatin1("tsmediaEmojiSearch"));
    d->search->setPlaceholderText(i18n::t("Search emoji"));
    d->search->setAccessibleName(i18n::t("Search emoji"));
    d->search->setClearButtonEnabled(true);
    d->search->setMaxLength(60);
    d->search->setFrame(false);
    const Colors& c = d->colors;
    // fromLatin1: TeamSpeak's style keeps the last parsed sheet text beyond this DLL's lifetime.
    d->search->setStyleSheet(QString::fromLatin1("QLineEdit{background:%1;color:%2;border:none;border-radius:6px;padding:0 6px;font-size:13px;"
                                                 "selection-background-color:%3;selection-color:#ffffff;}"
                                                 "QLineEdit:focus{border:2px solid %4;padding:0 4px;}")
                                 .arg(c.field.name(), c.fieldText.name(), c.accent.name(), c.ring.name()));
    {
        // The magnifier, drawn at the screen's pixel ratio.
        const qreal dpr = qMax(1.0, devicePixelRatioF());
        QPixmap     glass(QSize(16, 16) * dpr);
        glass.setDevicePixelRatio(dpr);
        glass.fill(Qt::transparent);
        QPainter gp(&glass);
        drawMagnifier(gp, QRectF(0, 0, 16, 16), c.placeholder);
        gp.end();
        d->search->addAction(QIcon(glass), QLineEdit::LeadingPosition);
    }
    QPalette pal = d->search->palette();
    pal.setColor(QPalette::PlaceholderText, c.placeholder);
    d->search->setPalette(pal);
    d->search->installEventFilter(this);

    d->toneButton = new ToneButton(&d->colors, this);
    d->toneButton->setTone(d->tone);

    d->grid = new EmojiGrid(&d->colors, this);

    d->tones = new TonePopup(&d->colors, this);
    d->place(); // 2.5 GIFs: through d->top (0 without a GIF host: as before)

    d->grid->onHover = [this](int item) {
        const int id = d->grid->idAt(item);
        if (id >= 0 && id != d->footerId) {
            d->footerId = id;
            update(d->footerRect());
        }
    };
    d->grid->onActivate = [this](int item, bool keep) { d->activate(item, keep); };
    d->grid->onScrolled = [this] { update(d->tabsRect()); };
    connect(d->search, &QLineEdit::textChanged, this, [this] { d->rebuild(); });
    connect(d->toneButton, &QAbstractButton::clicked, this, [this] { d->tones->open(d->tone); });
    d->tones->onPick = [this](int t) {
        d->tone = t;
        emoji::prefs::setTone(t);
        d->toneButton->setTone(t);
        d->rebuild();
        d->search->setFocus();
    };
    if (emoji::ImageNotifier* n = emoji::notifier())
        connect(n, &emoji::ImageNotifier::imagesReady, this, [this] { update(d->footerRect()); });

    d->grid->installEventFilter(new EmojiGridKeys(d, this));
    setTabOrder(d->search, d->toneButton);
    setTabOrder(d->toneButton, d->grid);
    d->rebuild();
}

EmojiPicker::~EmojiPicker()
{
    delete d->host;
    delete d->opened;
    delete d;
}

QSize EmojiPicker::preferredSize()
{
    return QSize(2 * kPad + kColumns * kCell + kBar, kPad + kSearch + 6 + kTabs + 5 + kGridH + 4 + kFooter);
}

QSize EmojiPicker::preferredSize(bool gifs)
{
    return preferredSize() + QSize(0, gifs ? kSwitchShift : 0);
}

void EmojiPicker::setGifHost(const gifs::Host& host)
{
    if (d->hosted || !host.library || d->mode != Mode::Insert)
        return;
    d->hosted = true;
    d->host   = new gifs::Host(host);
    d->opened = new gifs::SendState(host.state ? host.state() : gifs::SendState());
    d->top    = kSwitchShift;
    setFixedSize(preferredSize(true));
    d->place();
    setAccessibleName(i18n::t("Emoji and GIFs"));
    d->pickerKeys = new PickerKeys(d, this);
    d->switcher   = new PickerSwitch(d, this);
    d->switcher->setGeometry(kPad, kPad, kColumns * kCell - kMore - 4, kSwitchH);
    d->switcher->installEventFilter(d->pickerKeys);
    d->more = new MoreButton(&d->colors, this);
    d->more->move(width() - kPad - kMore, kPad + 2); // (348, 10)
    d->more->hide();
    d->more->installEventFilter(d->pickerKeys);
    d->toneButton->installEventFilter(d->pickerKeys);
    connect(d->more, &QAbstractButton::clicked, this, [this] {
        if (d->gifs)
            d->gifs->openMenu(d->more->mapToGlobal(QPoint(0, d->more->height())), QString());
    });
    setTabOrder(d->switcher, d->more);
    setTabOrder(d->more, d->search);
    // The chat tab changed behind the popup: it closes (a send would go elsewhere; design_gifs 6.1).
    if (host.chatTabs)
        connect(host.chatTabs.data(), &QTabBar::currentChanged, this, [this] { close(); });
}

bool EmojiPicker::hasGifHost() const
{
    return d->hosted;
}

EmojiPicker::Page EmojiPicker::page() const
{
    return d->page;
}

GifPanel* EmojiPicker::gifPanel() const
{
    return d->gifs;
}

QWidget* EmojiPicker::pageSwitch() const
{
    return d->switcher;
}

QWidget* EmojiPicker::moreButton() const
{
    return d->more;
}

void EmojiPicker::showPage(Page page)
{
    if (!d->hosted)
        return;
    d->page          = page;
    const bool emoji = page == Page::Emoji;
    d->search->setVisible(emoji);
    d->toneButton->setVisible(emoji);
    d->grid->setVisible(emoji);
    if (!emoji)
        d->tones->hide();
    d->hoverTab = -1;
    if (!emoji && !d->gifs) {
        // Created on first use; its snapshot of the list is taken now (design_gifs 5.3).
        d->gifs = new GifPanel(&d->colors, *d->host, *d->opened, this);
        d->gifs->setGeometry(0, kPad + kSwitchH + 6, width(), height() - (kPad + kSwitchH + 6));
        for (QWidget* w : d->gifs->keyWidgets())
            w->installEventFilter(d->pickerKeys);
        connect(d->gifs, &GifPanel::sent, this, [this](bool keepOpen) {
            if (!keepOpen)
                close();
        });
        connect(d->gifs, &GifPanel::typed, this, [this](const QString& text) {
            showPage(Page::Emoji);
            d->search->setFocus(Qt::OtherFocusReason);
            d->search->insert(text);
        });
        connect(d->gifs, &GifPanel::upFromGrid, this, [this] { d->switcher->setFocus(Qt::BacktabFocusReason); });
        connect(d->gifs, &GifPanel::closeRequested, this, [this] { close(); });
        setTabOrder(d->more, d->gifs->grid());
    }
    if (d->gifs) {
        d->gifs->setVisible(!emoji);
        d->gifs->setShown(!emoji);
        if (!emoji)
            d->gifs->refreshState();
    }
    d->more->setVisible(!emoji);
    emoji::prefs::setLastTabGifs(!emoji);
    static_cast<PickerSwitch*>(d->switcher)->updateAccessible();
    update();
}

void EmojiPicker::setMarked(const QSet<int>& ids)
{
    d->marked = ids;
    d->grid->setMarked(ids);
}

void EmojiPicker::setSearchText(const QString& text)
{
    d->search->setText(text);
}

void EmojiPicker::focusItem(int index)
{
    d->grid->setFocus(Qt::OtherFocusReason);
    d->grid->setFocusItem(index, true);
}

void EmojiPicker::scrollToGroup(int section)
{
    d->grid->scrollToSection(section);
    update(d->tabsRect());
}

void EmojiPicker::openAt(const QRect& anchor, bool preferBelow)
{
    const QSize size = this->size();
    QPoint      at(anchor.right() - size.width() + 1, preferBelow ? anchor.bottom() + 6 : anchor.top() - 6 - size.height());
    QScreen*    screen = QGuiApplication::screenAt(anchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (!preferBelow && at.y() < area.top())
            at.setY(anchor.bottom() + 6); // no room above: below it
        else if (preferBelow && at.y() + size.height() > area.bottom())
            at.setY(anchor.top() - 6 - size.height());
        at.setX(qBound(area.left(), at.x(), area.right() - size.width() + 1));
        at.setY(qBound(area.top(), at.y(), area.bottom() - size.height() + 1));
    }
    move(at);
    show();
    raise();
    activateWindow();
    if (d->page == Page::Gifs && d->gifs) {
        d->gifs->focusGrid(Qt::PopupFocusReason); // the grid, with no focused item: a stray Enter does nothing
        return;
    }
    d->search->setFocus(Qt::PopupFocusReason);
    d->grid->prefetchAround();
}

void EmojiPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(d->colors.border, 1.0));
    p.setBrush(d->colors.background);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setPen(QPen(d->colors.divider, 1.0));
    if (d->hosted) // 2.5 GIFs: under the switch row
        p.drawLine(QPointF(kPad, kPad + kSwitchH + 5.5), QPointF(width() - kPad, kPad + kSwitchH + 5.5));
    if (d->page == Page::Gifs)
        return; // the GIFs page paints its own grid and footer
    d->paintTabs(p);
    p.setPen(QPen(d->colors.divider, 1.0));
    const int line = d->tabsRect().bottom() + 2;
    p.drawLine(QPointF(kPad, line + 0.5), QPointF(width() - kPad, line + 0.5));
    d->paintFooter(p);
}

void EmojiPicker::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

void EmojiPicker::closeEvent(QCloseEvent* event)
{
    emit closed();
    QWidget::closeEvent(event);
}

bool EmojiPicker::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto*     he  = static_cast<QHelpEvent*>(event);
        const int tab = d->tabAt(he->pos());
        if (tab >= 0) {
            QToolTip::showText(he->globalPos(), d->tabName(tab), this, d->tabRect(tab));
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QWidget::event(event);
}

void EmojiPicker::mouseMoveEvent(QMouseEvent* event)
{
    const int tab = d->tabAt(event->pos());
    if (tab != d->hoverTab) {
        d->hoverTab = tab;
        setCursor(tab >= 0 && d->tabHasItems.value(tab) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update(d->tabsRect());
    }
}

void EmojiPicker::mouseReleaseEvent(QMouseEvent* event)
{
    const int tab = d->tabAt(event->pos());
    if (event->button() != Qt::LeftButton || tab < 0)
        return;
    if (!d->search->text().isEmpty())
        d->search->clear(); // the groups again
    if (d->tabSection.value(tab) >= 0) {
        d->grid->scrollToSection(d->tabSection.value(tab));
        update(d->tabsRect());
    }
}

void EmojiPicker::setOpener(const QRect& opener)
{
    d->opener = opener;
}

void EmojiPicker::mousePressEvent(QMouseEvent* event)
{
    // A press outside closes the popup, and Qt then gives the press to what is under it. On the button
    // that opened it that would open it again at once: a second click on the button closes it instead.
    if (!rect().contains(event->pos()) && d->opener.contains(event->globalPos()))
        setAttribute(Qt::WA_NoMouseReplay);
    QWidget::mousePressEvent(event);
}

void EmojiPicker::leaveEvent(QEvent*)
{
    if (d->hoverTab >= 0) {
        d->hoverTab = -1;
        update(d->tabsRect());
    }
}

bool EmojiPicker::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == d->search && event->type() == QEvent::ShortcutOverride
        && (isPickerShortcut(static_cast<QKeyEvent*>(event)) || (d->hosted && isPageSwitchKey(static_cast<QKeyEvent*>(event))))) {
        event->accept();
        return true;
    }
    if (watched == d->search && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (isPickerShortcut(ke)) {
            close();
            return true;
        }
        if (d->hosted && isPageSwitchKey(ke)) { // 2.5 GIFs
            d->togglePage();
            return true;
        }
        switch (ke->key()) {
        case Qt::Key_Down:
        case Qt::Key_Tab:
            if (ke->key() == Qt::Key_Tab && !d->search->text().isEmpty() && d->grid->itemCount() == 0)
                break;
            if (d->grid->itemCount() > 0) {
                d->grid->setFocus(Qt::TabFocusReason);
                d->grid->setFocusItem(d->grid->focus() < 0 ? 0 : d->grid->focus(), true);
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (d->grid->itemCount() > 0) {
                d->activate(d->grid->focus() < 0 ? 0 : d->grid->focus(), ke->modifiers() & Qt::ShiftModifier);
                return true;
            }
            return true;
        case Qt::Key_Escape:
            if (!d->search->text().isEmpty()) {
                d->search->clear();
                return true;
            }
            close();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

