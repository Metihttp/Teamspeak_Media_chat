#include "spoilersection.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "i18n.h"

SpoilerSection::SpoilerSection(QWidget* parent)
    : SettingsSection(parent)
{
    auto* group = new QGroupBox(i18n::t("Spoilers"), this);
    m_showAll   = new QCheckBox(i18n::t("Show spoilers without &blurring"), group);
    QLabel* help = hint(i18n::t("Pictures and videos marked as spoilers are shown right away. When this is off, they stay blurred until you click them."), group);
    m_showAll->setAccessibleDescription(help->text());

    auto* rows = form(group);
    rows->addRow(m_showAll);
    rows->addRow(help);
    addIndented(help); // lined up with the checkbox's text

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

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
