#include "chatlayoutsection.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

#include "i18n.h"

ChatLayoutSection::ChatLayoutSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaChatLayoutSection"));
    auto* group = new QGroupBox(i18n::t("Chat layout"), this);

    // Access keys: the General tab's others are taken (Clear cache, Microphone, ...).
    m_cozy    = new QRadioButton(i18n::t("Co&zy: names and pictures above messages, like Discord"), group);
    m_compact = new QRadioButton(i18n::t("Com&pact: one line per message, the time in front"), group);
    m_classic = new QRadioButton(i18n::t("&TeamSpeak classic: TeamSpeak's own look"), group);
    auto* choice = new QButtonGroup(group);
    choice->addButton(m_cozy, 1);
    choice->addButton(m_compact, 2);
    choice->addButton(m_classic, 0);
    QLabel* hintLabel = hint(i18n::t("Only your view changes. Others see the chat as before."), group);
    m_cozy->setAccessibleDescription(hintLabel->text());
    m_compact->setAccessibleDescription(hintLabel->text());
    m_classic->setAccessibleDescription(hintLabel->text());

    m_group = new QCheckBox(i18n::t("Group messages from the same person under one &name"), group);
    QLabel* groupHint = hint(i18n::t("Messages sent within 7 minutes of each other share one name and time, like in Discord."), group);
    m_group->setAccessibleDescription(groupHint->text());
    m_actions = new QCheckBox(i18n::t("Show message act&ions on hover"), group);
    QLabel* actionsHint = hint(i18n::t("Reply, Copy text and More over the message under the pointer. They are in the chat's right-click menu either way."), group);
    m_actions->setAccessibleDescription(actionsHint->text());

    // P2. Access keys v, y, r: free on the General tab.
    m_avatars = new QCheckBox(i18n::t("Show people's a&vatars (Cozy)"), group);
    QLabel* avatarsHint = hint(i18n::t("The pictures TeamSpeak already has. Nothing is downloaded; initials otherwise."), group);
    m_avatars->setAccessibleDescription(avatarsHint->text());
    m_mentions = new QCheckBox(i18n::t("Highlight messages that mention &you"), group);
    QLabel* mentionsHint = hint(i18n::t("Your name or @name in someone else's message."), group);
    m_mentions->setAccessibleDescription(mentionsHint->text());
    m_collapse = new QCheckBox(i18n::t("Fold runs of join and leave events into one &row"), group);
    QLabel* collapseHint = hint(i18n::t("The same event shows once with a count; four or more in a row show \"+N more events\". Pokes, kicks, bans and errors always show."), group);
    m_collapse->setAccessibleDescription(collapseHint->text());

    auto* rows = form(group);
    rows->addRow(m_cozy);
    rows->addRow(m_compact);
    rows->addRow(m_classic);
    rows->addRow(hintLabel);
    rows->addRow(m_group);
    rows->addRow(groupHint);
    rows->addRow(m_actions);
    rows->addRow(actionsHint);
    rows->addRow(m_avatars);
    rows->addRow(avatarsHint);
    rows->addRow(m_mentions);
    rows->addRow(mentionsHint);
    rows->addRow(m_collapse);
    rows->addRow(collapseHint);
    addIndented(groupHint);
    addIndented(actionsHint);
    addIndented(avatarsHint);
    addIndented(mentionsHint);
    addIndented(collapseHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

    for (QRadioButton* button : {m_cozy, m_compact, m_classic}) {
        connect(button, &QRadioButton::toggled, this, [this](bool on) {
            if (!on)
                return;
            updateEnabled();
            emit changed();
        });
    }
    connect(m_group, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_actions, &QCheckBox::toggled, this, &SettingsSection::changed);
    for (QCheckBox* box : {m_avatars, m_mentions, m_collapse})
        connect(box, &QCheckBox::toggled, this, &SettingsSection::changed);
}

int ChatLayoutSection::chosen() const
{
    return m_classic->isChecked() ? 0 : m_compact->isChecked() ? 2 : 1;
}

void ChatLayoutSection::updateEnabled()
{
    const bool modern = chosen() != 0;
    m_group->setEnabled(modern);
    m_actions->setEnabled(modern);
    m_avatars->setEnabled(chosen() == 1); // Cozy only
    m_mentions->setEnabled(modern);
    m_collapse->setEnabled(modern);
}

void ChatLayoutSection::load(const Settings& s)
{
    const QSignalBlocker a(m_cozy);
    const QSignalBlocker b(m_compact);
    const QSignalBlocker c(m_classic);
    const QSignalBlocker d(m_group);
    const QSignalBlocker e(m_actions);
    const QSignalBlocker f(m_avatars);
    const QSignalBlocker g(m_mentions);
    const QSignalBlocker h(m_collapse);
    m_cozy->setChecked(s.chatLayout == 1);
    m_compact->setChecked(s.chatLayout == 2);
    m_classic->setChecked(s.chatLayout == 0);
    m_group->setChecked(s.chatGroupMessages);
    m_actions->setChecked(s.chatHoverActions);
    m_avatars->setChecked(s.chatAvatars);
    m_mentions->setChecked(s.chatMentions);
    m_collapse->setChecked(s.chatCollapseEvents);
    updateEnabled();
}

void ChatLayoutSection::store(Settings& target, const Settings& loaded) const
{
    take(target.chatLayout, loaded.chatLayout, chosen());
    take(target.chatGroupMessages, loaded.chatGroupMessages, m_group->isChecked());
    take(target.chatHoverActions, loaded.chatHoverActions, m_actions->isChecked());
    take(target.chatAvatars, loaded.chatAvatars, m_avatars->isChecked());
    take(target.chatMentions, loaded.chatMentions, m_mentions->isChecked());
    take(target.chatCollapseEvents, loaded.chatCollapseEvents, m_collapse->isChecked());
}
