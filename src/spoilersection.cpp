#include "spoilersection.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QVBoxLayout>

#include "i18n.h"
#include "settingswidgets.h" // 2.7 F3

SpoilerSection::SpoilerSection(QWidget* parent)
    : SettingsSection(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // 2.7 (design 4.4): text spoilers are covered too, and hidden rather than blurred.
    QGroupBox* group = card(i18n::t("Spoilers"), this);
    m_showAll        = switchRow(group, i18n::t("Show spoilers without &blurring or hiding"),
                          i18n::t("Text, pictures and videos marked as spoilers are shown right away. When this is off, they stay hidden until you "
                                  "click them."));
    connect(m_showAll, &QCheckBox::toggled, this, &SettingsSection::changed);
}

void SpoilerSection::load(const Settings& s)
{
    const QSignalBlocker block(m_showAll);
    m_showAll->setChecked(s.revealSpoilers);
}

void SpoilerSection::store(Settings& target, const Settings& loaded) const
{
    take(target.revealSpoilers, loaded.revealSpoilers, m_showAll->isChecked());
}
