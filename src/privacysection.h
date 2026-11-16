#pragma once

// 2.2 protocol: Settings → Privacy → "Privacy": reactions and presence, both on by default. 2.7 F3 (M10): a link to the
// Link previews page, where the IP address question is answered.
// 2.4 link previews (2.7: LinkPreviewsSection, a page of its own, Settings → Link previews): showing previews of other
// people's web links, (2.5) cards for other web pages, loading the ones without a sender-made preview automatically,
// (2.7) only from your TeamSpeak friends, and making previews of your own: all on by default except Only from friends,
// each with a hint that says who contacts the website (also its accessible description); the lower ones are greyed out
// while the first is off (they need it), their ticks kept; Only from friends also needs "Load link previews
// automatically".
// 2.5 YouTube player: "Play YouTube videos in a TS Media window" (2.6: the window player's name; "in the chat" once the
// anchored player ships) below them, greyed out the same way.

#include "settingssection.h"

class QCheckBox;

class PrivacySection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit PrivacySection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    QCheckBox* m_reactions = nullptr;
    QCheckBox* m_presence  = nullptr;
};

class LinkPreviewsSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit LinkPreviewsSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

    // The hint under the player switch (with the runtime line from the registry check; tests).
    static QString playYouTubeHint();
    // 2.6 (design 8.2): the switch's text: "&Play YouTube videos in a TS Media window" while every Play is the window
    // player (ytpage::kAnchoredPlayer false), "&Play YouTube videos in the chat" with the anchored player. Key p kept.
    static QString playYouTubeLabel(bool anchored);

  private:
    void updateEnabled(); // everything below "Show previews ..." needs it; Only from friends also needs the automatic loads

    QCheckBox* m_showLinks   = nullptr; // 2.4 link previews
    QCheckBox* m_webCards    = nullptr; // 2.5: "Show cards for other web pages"
    QCheckBox* m_autoLinks   = nullptr;
    QCheckBox* m_onlyFriends = nullptr; // 2.7 F3: Settings::autoLoadLinkPreviewsOnlyFriends
    QCheckBox* m_makeLinks   = nullptr;
    QCheckBox* m_playYouTube = nullptr; // 2.5 YouTube player
};
