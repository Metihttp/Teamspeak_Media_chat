#pragma once

// 2.2 protocol: Settings > Privacy & updates > "Privacy": reactions and presence, both on by default.
// 2.4 link previews: "Link previews" below it: showing previews of other people's web links, loading the ones
// without a sender-made preview automatically, and making previews of your own, all on by default, each with a
// hint that says who contacts the website; the second and third are greyed out while the first is off (they
// need it).

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
    QCheckBox* m_autoLinks = nullptr;
    QLabel*    m_autoHint  = nullptr;
    QCheckBox* m_makeLinks = nullptr;
    QLabel*    m_makeHint  = nullptr;
};
