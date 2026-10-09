#include "composesettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "i18n.h"

ComposeSettingsSection::ComposeSettingsSection(QWidget* dialog)
    : SettingsSection(dialog)
{
    auto* group = new QGroupBox(i18n::t("Dropping files"), this);
    m_group     = group;
    m_dropMode  = new QComboBox(group);
    m_dropMode->addItem(i18n::t("Open the send window"));
    m_dropMode->addItem(i18n::t("Send right away"));
    auto* label = new QLabel(i18n::t("When you drop files on the c&hat"), group);
    label->setBuddy(m_dropMode);
    QLabel* help = hint(i18n::t("Hold Ctrl while dropping to do the other. Hold Shift for TeamSpeak's own drop."), group);
    m_dropMode->setAccessibleName(i18n::t("When you drop files on the chat"));
    m_dropMode->setAccessibleDescription(help->text());

    QFormLayout* form = SettingsSection::form(group);
    form->addRow(label, m_dropMode);
    addUnderField(form, help);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

    connect(m_dropMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_loading)
            emit changed();
    });

    // The dialog's own switch for dropping files (SettingsDialog names it): this only matters while it is on.
    if (dialog)
        m_dropSwitch = dialog->findChild<QCheckBox*>(QString::fromLatin1("dropSendsFiles"));
    if (m_dropSwitch)
        connect(m_dropSwitch, &QCheckBox::toggled, this, [this] { followDropSwitch(); });
    followDropSwitch();
}

void ComposeSettingsSection::load(const Settings& s)
{
    m_loading = true;
    m_dropMode->setCurrentIndex(s.dropOpensSendWindow ? 0 : 1);
    m_loading = false;
    followDropSwitch();
}

void ComposeSettingsSection::store(Settings& target, const Settings& loaded) const
{
    take(target.dropOpensSendWindow, loaded.dropOpensSendWindow, m_dropMode->currentIndex() == 0);
}

void ComposeSettingsSection::followDropSwitch()
{
    m_group->setEnabled(!m_dropSwitch || m_dropSwitch->isChecked());
}
