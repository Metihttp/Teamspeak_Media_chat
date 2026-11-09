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

    m_reactions           = new QCheckBox(i18n::t("S&how reactions"), group);
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

    // 2.4 link previews: who contacts the website, said plainly.
    auto* links = new QGroupBox(i18n::t("Link previews"), this);
    m_showLinks    = new QCheckBox(i18n::t("Show previews for links to &other websites"), links);
    auto* showHint = hint(i18n::t("Previews made by the sender come from the TeamSpeak server. Loading any other link's preview connects your PC to "
                                  "its website."),
                          links);
    m_showLinks->setAccessibleDescription(showHint->text());
    m_autoLinks = new QCheckBox(i18n::t("&Load link previews automatically"), links);
    m_autoHint  = hint(i18n::t("Links without a preview from their sender load it from the website by themselves, which sees your IP address: "
                               "YouTube always, pictures, GIFs and videos up to your automatic image download size (videos up to your video "
                               "size if you set one), nothing but YouTube with data saver. Off: only when you click \"Load preview\"."),
                       links);
    m_autoLinks->setAccessibleDescription(m_autoHint->text());
    m_makeLinks    = new QCheckBox(i18n::t("&Make previews for links I send"), links);
    m_makeHint     = hint(i18n::t("Your PC loads each link you send once and shares its preview through the TeamSpeak server."), links);
    m_makeLinks->setAccessibleDescription(m_makeHint->text());
    auto* linkForm = SettingsSection::form(links);
    linkForm->addRow(m_showLinks);
    linkForm->addRow(showHint);
    linkForm->addRow(m_autoLinks);
    linkForm->addRow(m_autoHint);
    linkForm->addRow(m_makeLinks);
    linkForm->addRow(m_makeHint);
    addIndented(showHint);
    addIndented(m_autoHint);
    addIndented(m_makeHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);
    layout->addWidget(links);

    connect(m_reactions, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_presence, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_showLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_showLinks, &QCheckBox::toggled, this, &PrivacySection::updateEnabled);
    connect(m_autoLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_makeLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    updateEnabled();
}

void PrivacySection::updateEnabled()
{
    // Loading automatically and your links' previews need the first box too (with it off nothing link-preview
    // related goes on, Settings): greyed out then, their ticks kept for when the first one comes back.
    const bool show = m_showLinks->isChecked();
    m_autoLinks->setEnabled(show);
    m_autoHint->setEnabled(show);
    m_makeLinks->setEnabled(show);
    m_makeHint->setEnabled(show);
}

void PrivacySection::load(const Settings& s)
{
    const QSignalBlocker a(m_reactions);
    const QSignalBlocker b(m_presence);
    const QSignalBlocker c(m_showLinks);
    const QSignalBlocker d(m_makeLinks);
    const QSignalBlocker e(m_autoLinks);
    m_reactions->setChecked(s.showReactions);
    m_presence->setChecked(s.sharePresence);
    m_showLinks->setChecked(s.showLinkPreviews);
    m_autoLinks->setChecked(s.autoLoadLinkPreviews);
    m_makeLinks->setChecked(s.makeLinkPreviews);
    updateEnabled();
}

void PrivacySection::store(Settings& target, const Settings& loaded) const
{
    take(target.showReactions, loaded.showReactions, m_reactions->isChecked());
    take(target.sharePresence, loaded.sharePresence, m_presence->isChecked());
    take(target.showLinkPreviews, loaded.showLinkPreviews, m_showLinks->isChecked());
    take(target.autoLoadLinkPreviews, loaded.autoLoadLinkPreviews, m_autoLinks->isChecked());
    take(target.makeLinkPreviews, loaded.makeLinkPreviews, m_makeLinks->isChecked());
}
