#include "appearancesection.h"

#include <QEvent>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "i18n.h"
#include "settingswidgets.h"

// 2.7 Dark mode (design_dark 3). Names built with fromLatin1 (CRASH RULE).

AppearanceSection::AppearanceSection(QWidget* parent, std::function<void(bool)> setDarkMode)
    : SettingsSection(parent)
    , m_setDarkMode(std::move(setDarkMode))
{
    setObjectName(QString::fromLatin1("tsmediaAppearanceSection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QGroupBox* group = card(i18n::t("Appearance"), this);
    m_switch         = switchRow(group, i18n::t("&Dark mode"),
                         i18n::t("Makes all of TeamSpeak dark right away: the main window, the chat, menus, dialogs and TS Media's windows. Turn "
                                         "it off to get your TeamSpeak style back exactly (Tools \xE2\x86\x92 Options \xE2\x86\x92 Design)."));
    m_switch->setObjectName(QString::fromLatin1("tsmediaDarkModeSwitch"));
    // Acts at once, never through OK / Apply: the hook saves and applies it, then the switch shows what is in effect.
    connect(m_switch, &QAbstractButton::clicked, this, [this](bool on) {
        if (m_setDarkMode)
            m_setDarkMode(on);
        refresh();
    });
    refresh();
}

void AppearanceSection::load(const Settings& s)
{
    Q_UNUSED(s);
    refresh();
}

void AppearanceSection::store(Settings& target, const Settings& loaded) const
{
    Q_UNUSED(target);
    Q_UNUSED(loaded);
}

void AppearanceSection::restoreDefaults(const Settings& defaults)
{
    Q_UNUSED(defaults);
}

void AppearanceSection::refresh()
{
    const bool on = Settings::instance().darkMode;
    if (m_switch->isChecked() != on) {
        const QSignalBlocker block(m_switch);
        m_switch->setChecked(on);
        m_switch->update();
    }
    const bool usable = static_cast<bool>(m_setDarkMode);
    if (m_switch->isEnabled() != usable)
        setRowEnabled(m_switch, usable);
}

bool AppearanceSection::event(QEvent* event)
{
    const bool done = SettingsSection::event(event);
    // The window became active again or the page came into view: the menu or the hotkey may have changed it meanwhile.
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::Show)
        refresh();
    return done;
}
