#include "proxygroup.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>

#include "i18n.h"
#include "proxytexts.h"
#include "proxyui.h"
#include "settingstheme.h"
#include "ts3api.h"
#include "uiutil.h"
#include "windowsproxy.h"

namespace proxy::ui {

ProxyGroup::ProxyGroup(QWidget* parent, Control* control)
    : QGroupBox(i18n::t("Chat proxy"), parent)
    , m_control(control ? control : Control::current())
{
    setObjectName(QString::fromLatin1("tsmediaProxyGroup"));
    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    QSizePolicy policy = m_status->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_status->setSizePolicy(policy);
    m_toggle   = new QPushButton(this);
    m_settings = new QPushButton(i18n::t("&Chat proxy settings…"), this);
    m_useIt    = new QPushButton(i18n::t("Use &it as the chat proxy"), this);
    for (QPushButton* b : {m_toggle, m_settings, m_useIt})
        b->setAutoDefault(false);
    m_useIt->hide();

    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(m_icon, 0, Qt::AlignTop);
    statusRow->addWidget(m_status, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_toggle);
    buttons->addWidget(m_settings);
    buttons->addWidget(m_useIt);
    buttons->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(4); // about three rows (F9): the Settings dialog grows by as little as it can
    layout->addLayout(statusRow);
    layout->addLayout(buttons);

    // At once, like Check now (not on OK / Apply); the line is printed like the menu's.
    connect(m_toggle, &QPushButton::clicked, this, [this] {
        if (!m_control)
            return;
        if (m_control->config().on)
            m_control->turnOff(Control::Origin::Settings, ts3::currentConnection());
        else
            m_control->turnOn(Control::Origin::Settings, ts3::currentConnection());
        refresh();
    });
    connect(m_settings, &QPushButton::clicked, this, [this] {
        // Nothing saved yet: the set-up form (the box ticked), as USAGE and the FAQ say (review fix).
        showProxyWindow(window(), settingsRequest(m_control && m_control->config().saved()), m_control);
    });
    connect(m_useIt, &QPushButton::clicked, this, [this] { showProxyWindow(window(), Request::UseIt, m_control); });
    if (m_control) {
        connect(m_control.data(), &Control::routeChanged, this, [this] { refresh(); });
        connect(m_control.data(), &Control::healthChanged, this, [this] { refresh(); });
    }
    refresh();
}

void ProxyGroup::refresh()
{
    if (!m_control)
        return;
    const Config saved = m_control->config();
    texts::StatusInput in;
    in.saved       = saved;
    in.mode        = m_control->mode();
    in.health      = m_control->state();
    in.lastFailure = m_control->lastFailure();
    in.windows     = m_control->windowsStatus();
    in.nowMs       = m_control->clockMs();
    in.summary     = windowsproxy::windowsProxySummary();
    in.group       = true;
    const Control::LastTest test = m_control->lastTest();
    if (test.ran && test.ok)
        in.lastTestWhen = ::ui::whenText(QDateTime::fromMSecsSinceEpoch(test.wallMs, Qt::UTC));
    const texts::StatusRow row = texts::statusRow(in);
    m_iconKind = row.icon == texts::StatusRow::Icon::Check ? QString::fromLatin1("check") : row.icon == texts::StatusRow::Icon::Warning ? QString::fromLatin1("warning") : QString();
    paintIcon();
    m_status->setText(row.text);
    m_status->setAccessibleDescription(row.text);
    // The Settings dialog's theme code colours the "ok" role (the check mark marks it too).
    m_status->setProperty("role", row.icon == texts::StatusRow::Icon::Check ? QString::fromLatin1("ok") : QString());
    m_toggle->setText(saved.on ? i18n::t("Turn off chat pro&xy") : i18n::t("Turn on chat pro&xy"));
    m_toggle->setVisible(saved.saved());
    m_useIt->setVisible(row.useIt);
    settingstheme::applyLabelRoles(this, settingstheme::roleColors(themePalette()));
}

QPalette ProxyGroup::themePalette() const
{
    // Live test fix: under TeamSpeak's dark skin the group box's own palette mixes the skin's light text with a light
    // Window (the skin paints backgrounds by style sheet), so its "ok" colour failed the contrast rule and the check mark
    // came out in the text colour (white) beside the green status text. The Settings dialog's palette is the one its role
    // style sheet colours that text from.
    const QWidget* top = window();
    return top ? top->palette() : palette();
}

void ProxyGroup::paintIcon()
{
    if (!m_icon)
        return; // a change event before the constructor made it
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    if (m_iconKind == QLatin1String("warning"))
        m_icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(size, size));
    else if (m_iconKind == QLatin1String("check"))
        m_icon->setPixmap(settingstheme::checkMark(settingstheme::roleColors(themePalette()).ok, size, devicePixelRatioF()));
    else
        m_icon->setPixmap(QPixmap());
    m_icon->setVisible(!m_iconKind.isEmpty());
}

void ProxyGroup::changeEvent(QEvent* event)
{
    QGroupBox::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        settingstheme::applyLabelRoles(this, settingstheme::roleColors(themePalette()));
        // Review fix: the check mark was painted with the palette of before the skin polished the Settings dialog (light
        // green on a dark skin): painted again in the colours of now.
        paintIcon();
    }
}

void ProxyGroup::showEvent(QShowEvent* event)
{
    QGroupBox::showEvent(event);
    paintIcon(); // polished by now
}

} // namespace proxy::ui
