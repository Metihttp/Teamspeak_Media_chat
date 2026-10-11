#include "emojisection.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "emojirender.h"
#include "i18n.h"

EmojiSection::EmojiSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaEmojiSection"));
    m_colour    = emoji::hasColor();
    auto* group = new QGroupBox(i18n::t("Emoji"), this);

    m_hd = new QCheckBox(i18n::t("Show emoji in high &quality in the chat"), group);
    QLabel* hdHint =
        hint(m_colour ? i18n::t("Emoji and TeamSpeak's smileys like :) show as sharp, colorful pictures. Everyone with TS Media sees them like this; "
                                "people without it see TeamSpeak's own.")
                      : i18n::t("This PC has no color emoji font (Segoe UI Emoji), so TeamSpeak's own emoji are shown."),
             group);
    m_hd->setAccessibleDescription(hdHint->text());

    m_jumbo = new QCheckBox(i18n::t("Show messages of only emoji lar&ge"), group);
    QLabel* jumboHint = hint(i18n::t("Up to 27 emoji with nothing else in the message, like in Discord."), group);
    m_jumbo->setAccessibleDescription(jumboHint->text());

    m_button = new QCheckBox(i18n::t("Use TS Media's emoji picker for TeamSpeak's emoji &button"), group);
    QLabel* buttonHint = hint(i18n::t("Off: TeamSpeak's own emoticon list. Ctrl+E opens TS Media's picker either way."), group);
    m_button->setAccessibleDescription(buttonHint->text());

    auto* rows = form(group);
    rows->addRow(m_hd);
    rows->addRow(hdHint);
    rows->addRow(m_jumbo);
    rows->addRow(jumboHint);
    rows->addRow(m_button);
    rows->addRow(buttonHint);
    addIndented(hdHint);
    addIndented(m_jumbo); // depends on the first one
    addIndented(jumboHint);
    addIndented(buttonHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

    connect(m_hd, &QCheckBox::toggled, this, [this] {
        updateEnabled();
        emit changed();
    });
    connect(m_jumbo, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_button, &QCheckBox::toggled, this, &SettingsSection::changed);
}

void EmojiSection::updateEnabled()
{
    m_hd->setEnabled(m_colour);
    m_jumbo->setEnabled(m_colour && m_hd->isChecked());
}

void EmojiSection::load(const Settings& s)
{
    const QSignalBlocker a(m_hd);
    const QSignalBlocker b(m_jumbo);
    const QSignalBlocker c(m_button);
    m_hd->setChecked(s.hdEmoji);
    m_jumbo->setChecked(s.jumboEmoji);
    m_button->setChecked(s.emojiButton);
    updateEnabled();
}

void EmojiSection::store(Settings& target, const Settings& loaded) const
{
    take(target.hdEmoji, loaded.hdEmoji, m_hd->isChecked());
    take(target.jumboEmoji, loaded.jumboEmoji, m_jumbo->isChecked());
    take(target.emojiButton, loaded.emojiButton, m_button->isChecked());
}
