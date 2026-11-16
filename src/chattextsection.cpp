#include "chattextsection.h"

#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <iterator>

#include "i18n.h"
#include "settingstheme.h"
#include "settingswidgets.h"

namespace {

// The preset tones (light background / dark background) for the swatches when the chat can't be asked (no hooks).
// The chat itself makes each one readable on its real background (design 4.2); the preview asks it when it can.
struct Tone {
    QRgb light, dark;
};
const Tone kTones[] = {{0x4752c4, 0x949cf7}, {0x0b5cad, 0x5eb0ff}, {0x0f766e, 0x3fc7b5}, {0x1e7b34, 0x5fcf80},
                       {0xa3510b, 0xf2a65a}, {0xa8326a, 0xf48fc0}};

const char* const kNames[] = {"Automatic", "Blue", "Teal", "Green", "Orange", "Pink", "Same as the chat text"};

QIcon swatch(const QColor& color, int size, qreal dpr, const QColor& ring)
{
    QPixmap pixmap(qMax(1, qRound(size * dpr)), qMax(1, qRound(size * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ring, 1));
    p.setBrush(color.isValid() ? color : QColor(Qt::transparent));
    p.drawEllipse(QRectF(0.5, 0.5, size - 1, size - 1));
    if (!color.isValid()) { // "Same as the chat text": an outlined circle with a slash
        p.setPen(QPen(ring, 1.2));
        p.drawLine(QPointF(size * 0.25, size * 0.75), QPointF(size * 0.75, size * 0.25));
    }
    return QIcon(pixmap);
}

} // namespace

ChatTextSection::ChatTextSection(QWidget* parent, ChatTextHooks hooks)
    : SettingsSection(parent)
    , m_hooks(std::move(hooks))
{
    setObjectName(QString::fromLatin1("tsmediaChatTextSection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const settingstheme::Tokens t     = settingstheme::tokensFor(this);
    QGroupBox*                  group = card(i18n::t("Text size and colors"), this);

    // Chat te&xt size: [-] 112% [+]  Re&set. Acts at once, like the wheel.
    auto* zoom = new QWidget(group);
    m_zoomRow  = zoom;
    m_smaller  = new IconButton(settingsicons::Icon::Minus, zoom);
    m_smaller->setAccessibleName(i18n::t("Smaller chat text"));
    m_smaller->setToolTip(i18n::t("Like Ctrl + mouse wheel down in the chat"));
    m_size = new QLabel(zoom);
    m_size->setAlignment(Qt::AlignCenter);
    m_size->setAccessibleName(i18n::t("Chat text size"));
    m_size->setMinimumWidth(m_size->fontMetrics().horizontalAdvance(QString::fromLatin1("999%")) + t.px(12));
    m_larger = new IconButton(settingsicons::Icon::Plus, zoom);
    m_larger->setAccessibleName(i18n::t("Larger chat text"));
    m_larger->setToolTip(i18n::t("Like Ctrl + mouse wheel up in the chat"));
    m_reset = new QPushButton(i18n::t("Re&set"), zoom);
    m_reset->setAutoDefault(false);
    m_reset->setAccessibleName(i18n::t("Reset the chat text size to 100%"));
    auto* zoomRow = new QHBoxLayout(zoom);
    zoomRow->setContentsMargins(0, t.px(4), 0, 0);
    zoomRow->setSpacing(t.px(4));
    zoomRow->addWidget(m_smaller);
    zoomRow->addWidget(m_size);
    zoomRow->addWidget(m_larger);
    zoomRow->addSpacing(t.px(8));
    zoomRow->addWidget(m_reset);
    zoomRow->addStretch(1);
    QLabel* zoomLabel = fieldRow(group, i18n::t("Chat te&xt size"), zoom,
                                 i18n::t("Like Ctrl + mouse wheel in the chat: changes every chat right away, and TS Media remembers it when TeamSpeak "
                                         "restarts. 100% is TeamSpeak's chat font (Tools \xE2\x86\x92 Options \xE2\x86\x92 Chat)."));
    // Alt+X focuses the row's first usable button and never clicks it: a QLabel clicks a buddy that is a button, and one
    // click here resizes every chat at once (review fix). The row takes no Tab stop of its own (ClickFocus) and hands
    // the focus on to its proxy (refreshZoom keeps the proxy on an enabled button).
    zoom->setFocusPolicy(Qt::ClickFocus);
    zoom->setFocusProxy(m_smaller);
    zoomLabel->setBuddy(zoom);
    zoom->setAccessibleDescription(QString());
    if (SettingRow* row = rowOf(zoom))
        row->setAlwaysStacked(true); // M9

    // &Color of messages from TS Media: a form value (OK / Apply), previewed at once.
    m_color = new QComboBox(group);
    for (int i = 0; i < static_cast<int>(std::size(kNames)); ++i) {
        const QString value = i == 0 ? QString() : QString::fromLatin1(Settings::lineColorChoices[i - 1]);
        m_color->addItem(i18n::t(kNames[i]), value);
    }
    m_color->setAccessibleName(i18n::t("Color of messages from TS Media"));
    fieldRow(group, i18n::t("&Color of messages from TS Media"), m_color,
             i18n::t("The \xE2\x80\x9CTS Media chat\xE2\x80\x9D lines that TS Media writes into your chat, and their links. Automatic picks a shade that "
                     "reads well on your chat's background, and every choice is made light or dark enough to stay readable. Lines already in the "
                     "chat keep their color."));
    if (SettingRow* row = rowOf(m_color))
        row->setAlwaysStacked(true); // M9
    m_preview = new LinePreview(group);
    m_preview->setAccessibleName(i18n::t("Preview of messages from TS Media in your chat"));
    customRow(group, m_preview);

    connect(m_smaller, &QPushButton::clicked, this, [this] { stepZoom(-1); });
    connect(m_larger, &QPushButton::clicked, this, [this] { stepZoom(+1); });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (m_hooks.setZoomTenths)
            m_hooks.setZoomTenths(0);
        refreshZoom();
    });
    connect(m_color, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        refreshPreview();
        if (!m_loading)
            emit changed();
    });
    refreshSwatches();
    refreshZoom();
}

QString ChatTextSection::choice() const
{
    return m_color->currentData().toString();
}

void ChatTextSection::load(const Settings& s)
{
    m_loading            = true;
    const QString wanted = Settings::normalizeLineColor(s.chatLineColor);
    int           index  = 0;
    for (int i = 0; i < m_color->count(); ++i) {
        if (m_color->itemData(i).toString() == wanted)
            index = i;
    }
    m_color->setCurrentIndex(index);
    m_loading = false;
    refreshZoom(); // the zoom isn't a form value: only read
}

void ChatTextSection::store(Settings& target, const Settings& loaded) const
{
    take(target.chatLineColor, loaded.chatLineColor, choice());
}

void ChatTextSection::stepZoom(int direction)
{
    if (!m_hooks.zoomTenths || !m_hooks.setZoomTenths)
        return;
    const int now = m_hooks.zoomTenths();
    m_hooks.setZoomTenths(qBound(Settings::chatZoomRange.min, now + direction * Settings::chatZoomStep, Settings::chatZoomRange.max));
    refreshZoom();
}

void ChatTextSection::refreshZoom()
{
    const bool    hooks   = m_hooks.zoomTenths && m_hooks.setZoomTenths;
    const int     tenths  = m_hooks.zoomTenths ? m_hooks.zoomTenths() : 0;
    const int     percent = m_hooks.zoomPercent ? m_hooks.zoomPercent() : 100;
    const QString value   = i18n::t("%1%").arg(percent);
    if (m_size->text() != value)
        m_size->setText(value);
    // A screen reader on the focused button hears the new size (DescriptionChanged).
    const QString description = i18n::t("Chat text size: %1%").arg(percent);
    for (QPushButton* b : {static_cast<QPushButton*>(m_smaller), static_cast<QPushButton*>(m_larger), m_reset}) {
        if (b->accessibleDescription() != description)
            b->setAccessibleDescription(description);
    }
    m_smaller->setEnabled(hooks && tenths > Settings::chatZoomRange.min);
    m_larger->setEnabled(hooks && tenths < Settings::chatZoomRange.max);
    m_reset->setEnabled(hooks && tenths != 0);
    QWidget* proxy = m_smaller;
    for (QWidget* b : {static_cast<QWidget*>(m_smaller), static_cast<QWidget*>(m_larger), static_cast<QWidget*>(m_reset)}) {
        if (b->isEnabled()) {
            proxy = b;
            break;
        }
    }
    if (m_zoomRow && m_zoomRow->focusProxy() != proxy)
        m_zoomRow->setFocusProxy(proxy);
    refreshPreview();
}

void ChatTextSection::refreshPreview()
{
    LinePreviewColors c;
    if (m_hooks.linePreview) {
        c = m_hooks.linePreview(choice());
    } else { // TeamSpeak's chat in the dialog's skin, in the Automatic colours (today's)
        const bool dark = settingstheme::tokensFor(this).dark;
        c.base          = dark ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white);
        c.text          = dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black);
        c.accent        = dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
        c.warning       = dark ? QColor(0xf0, 0xb2, 0x32) : QColor(0xb5, 0x47, 0x08);
    }
    m_preview->setColors(c.base, c.text, c.accent, c.warning);
}

void ChatTextSection::refreshColors()
{
    refreshSwatches();
    refreshPreview();
}

void ChatTextSection::refreshSwatches()
{
    const settingstheme::Tokens t   = settingstheme::tokensFor(this);
    const qreal                 dpr = devicePixelRatioF();
    m_swatchesDark                  = t.dark;
    m_swatchesDpr                   = dpr;
    for (int i = 0; i < m_color->count(); ++i) {
        QColor color;
        if (i < static_cast<int>(std::size(kTones))) {
            if (m_hooks.linePreview)
                color = m_hooks.linePreview(m_color->itemData(i).toString()).accent;
            else
                color = QColor(t.dark ? kTones[i].dark : kTones[i].light);
        }
        m_color->setItemIcon(i, swatch(color, t.px(14), dpr, t.control));
    }
    m_color->setIconSize(QSize(t.px(14), t.px(14)));
}

bool ChatTextSection::event(QEvent* event)
{
    const bool done = SettingsSection::event(event);
    // The window became active again, or the Chat page came into view: TeamSpeak's chat font (a new base, so a new
    // percentage without a zoom signal), the skin or the visible chat may have changed meanwhile (the syncVolume pattern).
    // ActivationChange reaches only the window itself; WindowActivate is passed on to its visible children (review fix).
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::Show) {
        if (m_hooks.linePreview)
            refreshSwatches();
        refreshZoom(); // and the preview
    }
    return done;
}

void ChatTextSection::changeEvent(QEvent* event)
{
    SettingsSection::changeEvent(event);
    switch (event->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange: {
        const settingstheme::Tokens t = settingstheme::tokensFor(this);
        if (t.dark != m_swatchesDark || !qFuzzyCompare(devicePixelRatioF(), m_swatchesDpr) || m_hooks.linePreview)
            refreshSwatches();
        refreshPreview();
        break;
    }
    default:
        break;
    }
}
