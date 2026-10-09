#include "voicesection.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "audio/wasapicapture.h"
#include "i18n.h"
#include "ts3api.h"

VoiceSection::VoiceSection(QWidget* parent)
    : SettingsSection(parent)
{
    auto* group  = new QGroupBox(i18n::t("Voice messages"), this);
    m_microphone = new QComboBox(group);
    m_microphone->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_microphone->setMinimumContentsLength(28);
    m_microphone->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); // the whole row: device names are long

    m_mute = new QCheckBox(i18n::t("M&ute my TeamSpeak microphone while recording"), group);
    QLabel* muteHint = hint(i18n::t("So people in your channel don't hear you live. Others see your microphone as muted until you finish."), group);
    m_mute->setAccessibleDescription(muteHint->text());

    m_review = new QCheckBox(i18n::t("&Let me listen before sending (hotkey)"), group);
    QLabel* reviewHint = hint(i18n::t("Pressing the hotkey again stops the recording so you can play it back. Turn off to send right away."), group);
    m_review->setAccessibleDescription(reviewHint->text());

    m_sounds = new QCheckBox(i18n::t("Play a sound when recording &starts and stops"), group);

    m_hotkey      = new QLabel(group);
    m_hotkey->setTextFormat(Qt::PlainText);
    auto* setup   = new QPushButton(i18n::t("Set up &hotkeys…"), group);
    setup->setAutoDefault(false);
    setup->setToolTip(i18n::t("Opens TeamSpeak's hotkey setup. Look for “Record a voice message” under TS Media chat."));
    auto* hotkeyRow = new QHBoxLayout;
    hotkeyRow->setContentsMargins(0, 0, 0, 0);
    hotkeyRow->addWidget(m_hotkey, 1);
    hotkeyRow->addWidget(setup);

    auto* rows = form(group);
    rows->addRow(i18n::t("&Microphone:"), m_microphone);
    rows->addRow(m_mute);
    rows->addRow(muteHint);
    rows->addRow(m_review);
    rows->addRow(reviewHint);
    rows->addRow(m_sounds);
    rows->addRow(hotkeyRow);
    addIndented(muteHint);
    addIndented(reviewHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

    connect(m_microphone, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsSection::changed);
    for (QCheckBox* box : {m_mute, m_review, m_sounds})
        connect(box, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(setup, &QPushButton::clicked, this, [] {
        if (ts3::funcs.showHotkeySetup)
            ts3::funcs.showHotkeySetup();
    });
    updateHotkey();
}

void VoiceSection::fillMicrophones(const QString& selected)
{
    const QSignalBlocker block(m_microphone);
    m_microphone->clear();
    m_microphone->addItem(i18n::t("Same as TeamSpeak (recommended)"), QString());
    int index = 0;
    for (const voice::Endpoint& e : voice::listCaptureEndpoints().endpoints) {
        m_microphone->addItem(e.name.isEmpty() ? i18n::t("Microphone") : e.name, e.id);
        if (!selected.isEmpty() && e.id.compare(selected, Qt::CaseInsensitive) == 0)
            index = m_microphone->count() - 1;
    }
    if (!selected.isEmpty() && index == 0) {
        // Picked earlier, unplugged now: kept (it is used again once it is back).
        m_microphone->addItem(i18n::t("The microphone picked earlier (not connected)"), selected);
        index = m_microphone->count() - 1;
    }
    m_microphone->setCurrentIndex(index);
}

void VoiceSection::load(const Settings& s)
{
    fillMicrophones(s.voiceMicrophone);
    const QSignalBlocker b1(m_mute);
    const QSignalBlocker b2(m_review);
    const QSignalBlocker b3(m_sounds);
    m_mute->setChecked(s.voiceMuteTeamSpeakMic);
    m_review->setChecked(s.voiceReview);
    m_sounds->setChecked(s.voiceSounds);
    updateHotkey();
}

void VoiceSection::store(Settings& target, const Settings& loaded) const
{
    // The id is copied (not shared with the combo's model) before it reaches the settings.
    const QString id = m_microphone->currentData().toString();
    take(target.voiceMicrophone, loaded.voiceMicrophone, QString(id.constData(), id.size()));
    take(target.voiceMuteTeamSpeakMic, loaded.voiceMuteTeamSpeakMic, m_mute->isChecked());
    take(target.voiceReview, loaded.voiceReview, m_review->isChecked());
    take(target.voiceSounds, loaded.voiceSounds, m_sounds->isChecked());
}

void VoiceSection::showEvent(QShowEvent* event)
{
    SettingsSection::showEvent(event);
    updateHotkey(); // set up in TeamSpeak's window meanwhile
}

void VoiceSection::updateHotkey()
{
    QString text;
    if (ts3::funcs.getHotkeyFromKeyword && !ts3::pluginId.isEmpty()) {
        const QByteArray id         = ts3::pluginId.toUtf8();
        const char*      keywords[] = {kHotkeyKeyword};
        char             buffer[128] = {};
        char*            hotkeys[]  = {buffer};
        if (ts3::funcs.getHotkeyFromKeyword(id.constData(), keywords, hotkeys, 1, sizeof(buffer)) == ERROR_ok)
            text = QString::fromUtf8(buffer).trimmed();
    }
    m_hotkey->setText(text.isEmpty() ? i18n::t("Record hotkey: not set") : i18n::t("Record hotkey: %1").arg(text));
}
