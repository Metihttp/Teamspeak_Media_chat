#pragma once

// 2.2 protocol: Settings > Privacy & updates > "Privacy": reactions and presence, both on by default.
// 2.4 link previews: "Link previews" below it: showing previews of other people's web links, (2.5) cards for other
// web pages, loading the ones without a sender-made preview automatically, and making previews of your own: four
// boxes, all on by default, each with a hint that says who contacts the website (also its accessible
// description); the lower three are greyed out while the first is off (they need it), their ticks kept.
// 2.5 YouTube player: "Play YouTube videos in a TS Media window" (2.6: the window player's name; "in the chat" once the
// anchored player ships) below them, greyed out the same way.

#include "settingssection.h"

class QCheckBox;
class QLabel;

class PrivacySection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit PrivacySection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    void updateEnabled(); // the automatic loads and "Make previews for links I send" need "Show previews ..." on

    QCheckBox* m_reactions;
    QCheckBox* m_presence;
    QCheckBox* m_showLinks = nullptr; // 2.4 link previews
    QCheckBox* m_webCards  = nullptr; // 2.5: "Show cards for other web pages"
    QLabel*    m_webHint   = nullptr;
    QCheckBox* m_autoLinks = nullptr;
    QLabel*    m_autoHint  = nullptr;
    QCheckBox* m_makeLinks = nullptr;
    QLabel*    m_makeHint  = nullptr;
    QCheckBox* m_playYouTube = nullptr; // 2.5 YouTube player
    QLabel*    m_playHint    = nullptr;

  public:
    // The hint under the player switch (with the runtime line from the registry check; tests).
    static QString playYouTubeHint();
    // 2.6 (design 8.2): the switch's text: "&Play YouTube videos in a TS Media window" while every Play is the window
    // player (ytpage::kAnchoredPlayer false), "&Play YouTube videos in the chat" with the anchored player. Key p kept.
    static QString playYouTubeLabel(bool anchored);
};
