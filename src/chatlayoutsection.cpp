#include "chatlayoutsection.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

#include "i18n.h"
#include "settingstheme.h"   // 2.7 F3
#include "settingswidgets.h" // 2.7 F3: tiles and switches

ChatLayoutSection::ChatLayoutSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaChatLayoutSection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(settingstheme::tokensFor(this).px(18));

    // 2.7 F3: the layouts as picture tiles (still radio buttons with their 2.6 texts and access keys z, p, t).
    QGroupBox* group = card(i18n::t("Chat layout"), this);
    auto*      tiles = new QWidget(group);
    auto*      tileRow = new TileLayout(tiles);
    m_cozy    = new LayoutTile(LayoutTile::Kind::Cozy, i18n::t("Co&zy: names and pictures above messages, like Discord"), tiles);
    m_compact = new LayoutTile(LayoutTile::Kind::Compact, i18n::t("Com&pact: one line per message, the time in front"), tiles);
    m_classic = new LayoutTile(LayoutTile::Kind::Classic, i18n::t("&TeamSpeak classic: TeamSpeak's own look"), tiles);
    tileRow->addWidget(m_cozy);
    tileRow->addWidget(m_compact);
    tileRow->addWidget(m_classic);
    auto* choice = new QButtonGroup(group);
    choice->addButton(m_cozy, 1);
    choice->addButton(m_compact, 2);
    choice->addButton(m_classic, 0);
    customRow(group, tiles);
    QLabel* hintLabel = hint(i18n::t("Only your view changes. Others see the chat as before."), group);
    customRow(group, hintLabel);
    m_cozy->setAccessibleDescription(hintLabel->text());
    m_compact->setAccessibleDescription(hintLabel->text());
    m_classic->setAccessibleDescription(hintLabel->text());

    // What the layout does (Cozy and Compact). Access keys: the Chat page's others are the tiles' and the text size
    // card's (x, c, s); 2.7: "events" (e) instead of "row" (r, which is the dialog's Restore defaults).
    QGroupBox* messages = card(i18n::t("Messages"), this);
    m_group   = switchRow(messages, i18n::t("Group messages from the same person under one &name"),
                          i18n::t("Messages sent within 7 minutes of each other share one name and time, like in Discord."));
    m_actions = switchRow(messages, i18n::t("Show message act&ions on hover"),
                          i18n::t("Reply, Copy text and More over the message under the pointer. They are in the chat's right-click menu either way."));
    // 2.3 chat-visuals (access key u: b is the emoji button's)
    m_bubbles = switchRow(messages, i18n::t("Show messages in b&ubbles (Cozy)"),
                          i18n::t("Each message on its own rounded background, yours tinted, like in Telegram. Events sit centred on small pills."));
    // 2.3.1, under bubbles (it needs them)
    m_ownRight = switchRow(messages, i18n::t("Show &my messages on the right"),
                           i18n::t("Like in Telegram: your messages sit on the right without your name or picture, others' on the left. Cozy with "
                                   "bubbles only."),
                           1);
    m_avatars  = switchRow(messages, i18n::t("Show people's a&vatars (Cozy)"),
                           i18n::t("TeamSpeak's own pictures of people, yours too; initials otherwise. TS Media downloads nothing.")); // 2.3 chat-visuals
    m_mentions = switchRow(messages, i18n::t("Highlight messages that mention &you"), i18n::t("Your name or @name in someone else's message."));
    m_collapse = switchRow(messages, i18n::t("Fold runs of join and leave &events into one row"),
                           i18n::t("The same event shows once with a count; four or more in a row show \"+N more events\". Pokes, kicks, bans and "
                                   "errors always show."));

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
    for (QCheckBox* box : {m_avatars, m_mentions, m_collapse, m_bubbles, m_ownRight}) // 2.3 chat-visuals: bubbles; 2.3.1
        connect(box, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_bubbles, &QCheckBox::toggled, this, [this] { updateEnabled(); }); // 2.3.1: on the right needs bubbles
}

int ChatLayoutSection::chosen() const
{
    return m_classic->isChecked() ? 0 : m_compact->isChecked() ? 2 : 1;
}

void ChatLayoutSection::updateEnabled()
{
    // 2.7 F3: the whole row (title, helper, switch) greys out; the tick is kept.
    const bool modern = chosen() != 0;
    setRowEnabled(m_group, modern);
    setRowEnabled(m_actions, modern);
    setRowEnabled(m_avatars, chosen() == 1); // Cozy only
    setRowEnabled(m_bubbles, chosen() == 1); // 2.3 chat-visuals: Cozy only
    setRowEnabled(m_ownRight, chosen() == 1 && m_bubbles->isChecked()); // 2.3.1: Cozy with bubbles only
    setRowEnabled(m_mentions, modern);
    setRowEnabled(m_collapse, modern);
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
    const QSignalBlocker i(m_bubbles); // 2.3 chat-visuals
    const QSignalBlocker j(m_ownRight); // 2.3.1
    m_cozy->setChecked(s.chatLayout == 1);
    m_compact->setChecked(s.chatLayout == 2);
    m_classic->setChecked(s.chatLayout == 0);
    m_group->setChecked(s.chatGroupMessages);
    m_actions->setChecked(s.chatHoverActions);
    m_avatars->setChecked(s.chatAvatars);
    m_mentions->setChecked(s.chatMentions);
    m_collapse->setChecked(s.chatCollapseEvents);
    m_bubbles->setChecked(s.chatBubbles); // 2.3 chat-visuals
    m_ownRight->setChecked(s.chatOwnRight); // 2.3.1
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
    take(target.chatBubbles, loaded.chatBubbles, m_bubbles->isChecked()); // 2.3 chat-visuals
    take(target.chatOwnRight, loaded.chatOwnRight, m_ownRight->isChecked()); // 2.3.1
}
