#include "privacysection.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "i18n.h"
#include "ytplayer/ytbackend.h" // 2.5 YouTube player: the WebView2 Runtime check (registry only)
#include "ytplayer/ytpage.h"    // the release gate of the player over the card (its wording)

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

    // 2.4 link previews: who contacts the website, said plainly. 2.5: cards for other web pages have a box of their own.
    auto* links = new QGroupBox(i18n::t("Link previews"), this);
    m_showLinks    = new QCheckBox(i18n::t("Show previews for links to &other websites"), links);
    auto* showHint = hint(i18n::t("Videos, GIFs and pictures, cards for well-known sites such as Vimeo, Spotify, X or GitHub, and cards for other web "
                                  "pages. Previews made by the sender come from the TeamSpeak server; loading any other link's preview connects your "
                                  "PC to its website."),
                          links);
    m_showLinks->setAccessibleDescription(showHint->text());
    m_webCards = new QCheckBox(i18n::t("Show cards for other &web pages"), links);
    // design2 2.12: share-shaped links are still loaded with it off (they may hold a picture, GIF or video).
    m_webHint  = hint(i18n::t("A title, a short text and a picture read from the page itself, for links that aren't videos, GIFs, pictures or "
                              "well-known sites. Off: those links get no card and your PC doesn't load those pages (except links shaped like a "
                              "Nextcloud or ownCloud share, \xE2\x80\xA6/s/\xE2\x80\xA6, which are still loaded to learn whether they hold a "
                              "picture, GIF or video)."),
                      links);
    m_webCards->setAccessibleDescription(m_webHint->text());
    m_autoLinks = new QCheckBox(i18n::t("&Load link previews automatically"), links);
    m_autoHint  = hint(i18n::t("Links without a preview from their sender load it from the website by themselves when they scroll into view, which "
                               "sees your IP address: cards always (with data saver smaller ones); pictures, GIFs and videos up to your automatic "
                               "image download size (videos up to your video size if you set one), and with data saver none. Off: only when you "
                               "click \"Load preview\", or \"Load link previews\" in a message's menu."),
                       links);
    m_autoLinks->setAccessibleDescription(m_autoHint->text());
    m_makeLinks    = new QCheckBox(i18n::t("&Make previews for links I send"), links);
    m_makeHint     = hint(i18n::t("Your PC loads each link you send once (the page and its picture, or the file) and shares its preview through the "
                                  "TeamSpeak server."),
                          links);
    m_makeLinks->setAccessibleDescription(m_makeHint->text());
    // 2.5 YouTube player: YouTube's own player over the card (Microsoft Edge WebView2). Whether this PC has the WebView2
    // Runtime is read from the registry when the dialog opens (never by loading the player helper).
    ytbackend::refreshRuntime();
    m_playYouTube = new QCheckBox(i18n::t("&Play YouTube videos in the chat"), links);
    m_playYouTube->setObjectName(QString::fromLatin1("tsmediaPlayYouTube"));
    m_playHint    = hint(playYouTubeHint(), links);
    m_playYouTube->setAccessibleDescription(m_playHint->text());
    auto* linkForm = SettingsSection::form(links);
    linkForm->addRow(m_showLinks);
    linkForm->addRow(showHint);
    linkForm->addRow(m_webCards);
    linkForm->addRow(m_webHint);
    linkForm->addRow(m_autoLinks);
    linkForm->addRow(m_autoHint);
    linkForm->addRow(m_makeLinks);
    linkForm->addRow(m_makeHint);
    linkForm->addRow(m_playYouTube);
    linkForm->addRow(m_playHint);
    addIndented(showHint);
    addIndented(m_webHint);
    addIndented(m_autoHint);
    addIndented(m_makeHint);
    addIndented(m_playHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);
    layout->addWidget(links);

    connect(m_reactions, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_presence, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_showLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_showLinks, &QCheckBox::toggled, this, &PrivacySection::updateEnabled);
    connect(m_webCards, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_autoLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_makeLinks, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_playYouTube, &QCheckBox::toggled, this, &SettingsSection::changed);
    updateEnabled();
}

void PrivacySection::updateEnabled()
{
    // Cards for other pages, loading automatically and your links' previews need the first box too (with it off
    // nothing link-preview related goes on, Settings): greyed out then, their ticks kept for when it comes back.
    const bool show = m_showLinks->isChecked();
    m_webCards->setEnabled(show);
    m_webHint->setEnabled(show);
    m_autoLinks->setEnabled(show);
    m_autoHint->setEnabled(show);
    m_makeLinks->setEnabled(show);
    m_makeHint->setEnabled(show);
    m_playYouTube->setEnabled(show);
    m_playHint->setEnabled(show);
}

QString PrivacySection::playYouTubeHint()
{
    // Where the player shows follows the release gate (2.5.0: a window of its own over TeamSpeak), so it can't drift.
    QString text;
    if constexpr (ytpage::kAnchoredPlayer)
        text = i18n::t("Play shows YouTube's own player over the card, in Microsoft Edge WebView2, using YouTube's privacy-enhanced mode (which "
                       "YouTube describes for websites). The player loads nothing before you press Play; the card's title and picture come "
                       "from YouTube as described under Link previews, unless the sender's plugin made the preview. Off: Play opens the video "
                       "in your browser.");
    else
        text = i18n::t("Play shows YouTube's own player in a window of its own over TeamSpeak, in Microsoft Edge WebView2, using YouTube's "
                       "privacy-enhanced mode (which YouTube describes for websites). The player loads nothing before you press Play; the "
                       "card's title and picture come from YouTube as described under Link previews, unless the sender's plugin made the "
                       "preview. Off: Play opens the video in your browser.");
    if (!ytbackend::helperBuiltIn())
        text += QLatin1Char(' ') + (sizeof(void*) == 4 ? i18n::t("In 32-bit TeamSpeak, videos open in your browser.")
                                                      : i18n::t("This copy of TS Media has no YouTube player, so videos open in your browser."));
    else if (!ytbackend::runtimePresent())
        text += QLatin1Char(' ') + i18n::t("This PC doesn't have the Microsoft Edge WebView2 Runtime, so videos open in your browser.");
    return text;
}

void PrivacySection::load(const Settings& s)
{
    const QSignalBlocker a(m_reactions);
    const QSignalBlocker b(m_presence);
    const QSignalBlocker c(m_showLinks);
    const QSignalBlocker d(m_makeLinks);
    const QSignalBlocker e(m_autoLinks);
    const QSignalBlocker f(m_webCards);
    const QSignalBlocker g(m_playYouTube);
    m_reactions->setChecked(s.showReactions);
    m_presence->setChecked(s.sharePresence);
    m_showLinks->setChecked(s.showLinkPreviews);
    m_webCards->setChecked(s.showWebCards);
    m_autoLinks->setChecked(s.autoLoadLinkPreviews);
    m_makeLinks->setChecked(s.makeLinkPreviews);
    m_playYouTube->setChecked(s.playYouTubeInChat);
    updateEnabled();
}

void PrivacySection::store(Settings& target, const Settings& loaded) const
{
    take(target.showReactions, loaded.showReactions, m_reactions->isChecked());
    take(target.sharePresence, loaded.sharePresence, m_presence->isChecked());
    take(target.showLinkPreviews, loaded.showLinkPreviews, m_showLinks->isChecked());
    take(target.showWebCards, loaded.showWebCards, m_webCards->isChecked());
    take(target.autoLoadLinkPreviews, loaded.autoLoadLinkPreviews, m_autoLinks->isChecked());
    take(target.makeLinkPreviews, loaded.makeLinkPreviews, m_makeLinks->isChecked());
    take(target.playYouTubeInChat, loaded.playYouTubeInChat, m_playYouTube->isChecked()); // 2.5 (never youTubeNoticeShown)
}
