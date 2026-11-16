#include "privacysection.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include "i18n.h"
#include "settingsdialog.h"     // 2.7 F3 (M10): the Link previews page's Tab value
#include "settingswidgets.h"    // 2.7 F3
#include "ytplayer/ytbackend.h" // 2.5 YouTube player: the WebView2 Runtime check (registry only)
#include "ytplayer/ytpage.h"    // the release gate of the player over the card (its wording)

PrivacySection::PrivacySection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaPrivacySection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QGroupBox* group = card(i18n::t("Privacy"), this);

    m_reactions = switchRow(group, i18n::t("S&how reactions"),
                            i18n::t("Reactions travel through TeamSpeak to people with TS Media who are online in the same chat. They aren't stored on "
                                    "the server."));
    m_presence  = switchRow(group, i18n::t("Tell people in your channel that &you have TS Media"),
                           i18n::t("Lets the send window show who will see your media in the chat. Your TS Media version is shared with people in "
                                   "your channel and with private-chat partners you send to. Nothing leaves TeamSpeak."));
    // 2.7 F3 (M10): the IP-grabber answer lives on the Link previews page; people look for it here.
    auto* holder  = new QWidget(group);
    auto* toLinks = new QPushButton(i18n::t("&Link previews and your IP address…"), holder);
    toLinks->setProperty("tsmediaLink", true); // drawn as a link by the dialog's sheet
    toLinks->setAutoDefault(false);
    toLinks->setCursor(Qt::PointingHandCursor);
    toLinks->setObjectName(QString::fromLatin1("tsmediaPrivacyLinkPreviews"));
    toLinks->setAccessibleDescription(i18n::t("Opens the Link previews page: which previews load by themselves, and from whom."));
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(toLinks);
    row->addStretch(1);
    customRow(group, holder);

    connect(m_reactions, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_presence, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(toLinks, &QPushButton::clicked, this, [this] { emit showPage(static_cast<int>(SettingsDialog::Tab::LinkPreviews)); });
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

LinkPreviewsSection::LinkPreviewsSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaLinkPreviewsSection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // 2.4 link previews: who contacts the website, said plainly. 2.5: cards for other web pages have a switch of their own.
    QGroupBox* links = card(i18n::t("Link previews"), this);
    m_showLinks      = switchRow(links, i18n::t("Show previews for links to &other websites"),
                            i18n::t("Videos, GIFs and pictures, cards for well-known sites such as Vimeo, Spotify, X or GitHub, and cards for other "
                                    "web pages. Previews made by the sender come from the TeamSpeak server; loading any other link's preview "
                                    "connects your PC to its website."));
    // design2 2.12: share-shaped links are still loaded with it off (they may hold a picture, GIF or video).
    m_webCards = switchRow(links, i18n::t("Show cards for other &web pages"),
                           i18n::t("A title, a short text and a picture read from the page itself, for links that aren't videos, GIFs, pictures or "
                                   "well-known sites. Off: those links get no card and your PC doesn't load those pages (except links shaped like a "
                                   "Nextcloud or ownCloud share, \xE2\x80\xA6/s/\xE2\x80\xA6, which are still loaded to learn whether they hold a "
                                   "picture, GIF or video)."),
                           1);
    // 2.7 (design 4.4): the 2.6 hint and the Blocked sentence.
    m_autoLinks = switchRow(links, i18n::t("&Load link previews automatically"),
                            i18n::t("Links without a preview from their sender load it from the website by themselves when they scroll into view, "
                                    "which sees your IP address: cards always (with data saver smaller ones); pictures, GIFs and videos up to your "
                                    "automatic image download size (videos up to your video size if you set one), and with data saver none. Off: "
                                    "only when you click \"Load preview\", or \"Load link previews\" in a message's menu. Links from people you "
                                    "blocked in TeamSpeak never load by themselves."),
                            1);
    // 2.7 F2/F3: Only from friends, under the automatic loads (it needs them). No "automatically" in its text.
    m_onlyFriends = switchRow(links, i18n::t("Only from &friends"),
                              i18n::t("Links from people you marked as Friend in TeamSpeak (right-click a name, then Add as Friend, or Tools "
                                      "\xE2\x86\x92 Contacts) load by themselves; everyone else's show \"Load preview\". Previews made by the sender "
                                      "(they come from the TeamSpeak server and never connect your PC to the website) and your own links "
                                      "aren't affected."),
                              2);
    m_onlyFriends->setToolTip(i18n::t("TS Media reads your TeamSpeak contacts on this computer, read-only. Nothing is sent."));
    m_makeLinks = switchRow(links, i18n::t("&Make previews for links I send"),
                            i18n::t("Your PC loads each link you send once (the page and its picture, or the file) and shares its preview through the "
                                    "TeamSpeak server."),
                            1);
    // 2.5 YouTube player: YouTube's own player over the card (Microsoft Edge WebView2). Whether this PC has the WebView2
    // Runtime is read from the registry when the dialog opens (never by loading the player helper).
    ytbackend::refreshRuntime();
    m_playYouTube = switchRow(links, playYouTubeLabel(ytpage::kAnchoredPlayer), playYouTubeHint(), 1);
    m_playYouTube->setObjectName(QString::fromLatin1("tsmediaPlayYouTube"));

    for (QCheckBox* box : {m_showLinks, m_webCards, m_autoLinks, m_onlyFriends, m_makeLinks, m_playYouTube})
        connect(box, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_showLinks, &QCheckBox::toggled, this, &LinkPreviewsSection::updateEnabled);
    connect(m_autoLinks, &QCheckBox::toggled, this, &LinkPreviewsSection::updateEnabled);
    updateEnabled();
}

void LinkPreviewsSection::updateEnabled()
{
    // Cards for other pages, loading automatically and your links' previews need the first switch too (with it off
    // nothing link-preview related goes on, Settings): greyed out then, their ticks kept for when it comes back.
    // Only from friends limits the automatic loads: it needs both.
    const bool show = m_showLinks->isChecked();
    setRowEnabled(m_webCards, show);
    setRowEnabled(m_autoLinks, show);
    setRowEnabled(m_onlyFriends, show && m_autoLinks->isChecked());
    setRowEnabled(m_makeLinks, show);
    setRowEnabled(m_playYouTube, show);
}

QString LinkPreviewsSection::playYouTubeLabel(bool anchored)
{
    return anchored ? i18n::t("&Play YouTube videos in the chat") : i18n::t("&Play YouTube videos in a TS Media window");
}

QString LinkPreviewsSection::playYouTubeHint()
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

void LinkPreviewsSection::load(const Settings& s)
{
    const QSignalBlocker c(m_showLinks);
    const QSignalBlocker d(m_makeLinks);
    const QSignalBlocker e(m_autoLinks);
    const QSignalBlocker f(m_webCards);
    const QSignalBlocker g(m_playYouTube);
    const QSignalBlocker h(m_onlyFriends);
    m_showLinks->setChecked(s.showLinkPreviews);
    m_webCards->setChecked(s.showWebCards);
    m_autoLinks->setChecked(s.autoLoadLinkPreviews);
    m_onlyFriends->setChecked(s.autoLoadLinkPreviewsOnlyFriends); // 2.7
    m_makeLinks->setChecked(s.makeLinkPreviews);
    m_playYouTube->setChecked(s.playYouTubeInChat);
    updateEnabled();
}

void LinkPreviewsSection::store(Settings& target, const Settings& loaded) const
{
    take(target.showLinkPreviews, loaded.showLinkPreviews, m_showLinks->isChecked());
    take(target.showWebCards, loaded.showWebCards, m_webCards->isChecked());
    take(target.autoLoadLinkPreviews, loaded.autoLoadLinkPreviews, m_autoLinks->isChecked());
    take(target.autoLoadLinkPreviewsOnlyFriends, loaded.autoLoadLinkPreviewsOnlyFriends, m_onlyFriends->isChecked()); // 2.7
    take(target.makeLinkPreviews, loaded.makeLinkPreviews, m_makeLinks->isChecked());
    take(target.playYouTubeInChat, loaded.playYouTubeInChat, m_playYouTube->isChecked()); // 2.5 (never youTubeNoticeShown)
}
