#include "privacysection.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "i18n.h"

PrivacySection::PrivacySection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaPrivacySection"));
    auto* group = new QGroupBox(i18n::t("Privacy"), this);

    m_reactions           = new QCheckBox(i18n::t("S&how reactions on media"), group);
    auto* reactionsHint   = hint(i18n::t("Reactions travel through TeamSpeak to people with TS Media who are online in the same chat. They aren't stored on the server."), group);
    m_reactions->setAccessibleDescription(reactionsHint->text());

    m_presence         = new QCheckBox(i18n::t("Tell people in your channel that &you have TS Media"), group);
    auto* presenceHint = hint(i18n::t("Lets the send window show who will see your media in the chat. Your TS Media version is shared with people in your "
                                      "channel and with private-chat partners you send to. Nothing leaves TeamSpeak."),
                              group);
    m_presence->setAccessibleDescription(presenceHint->text());

    auto* form = SettingsSection::form(group);
    form->addRow(m_reactions);
    form->addRow(reactionsHint);
    form->addRow(m_presence);
    form->addRow(presenceHint);
    addIndented(reactionsHint);
    addIndented(presenceHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

    connect(m_reactions, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_presence, &QCheckBox::toggled, this, &SettingsSection::changed);
}

void PrivacySection::load(const Settings& s)
{
    const QSignalBlocker a(m_reactions);
    const QSignalBlocker b(m_presence);
    m_reactions->setChecked(s.showReactions);
    m_presence->setChecked(s.sharePresence);
}

void PrivacySection::store(Settings& target, const Settings& loaded) const
{
    take(target.showReactions, loaded.showReactions, m_reactions->isChecked());
    take(target.sharePresence, loaded.sharePresence, m_presence->isChecked());
}
