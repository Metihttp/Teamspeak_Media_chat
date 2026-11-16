#include "emojisection.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "emojirender.h"
#include "giflibrary.h" // 2.5 GIFs
#include "gifpanel.h"   // 2.6: the Remove button's text
#include "i18n.h"
#include "settingstheme.h"   // 2.7 F3
#include "settingswidgets.h" // 2.7 F3

EmojiSection::EmojiSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaEmojiSection"));
    m_colour     = emoji::hasColor();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(settingstheme::tokensFor(this).px(18));

    QGroupBox* group = card(i18n::t("Emoji"), this);
    m_hd             = switchRow(group, i18n::t("Show emoji in high &quality in the chat"),
                     m_colour ? i18n::t("Emoji and TeamSpeak's smileys like :) show as sharp, colorful pictures. Everyone with TS Media sees them like "
                                        "this; people without it see TeamSpeak's own.")
                              : i18n::t("This PC has no color emoji font (Segoe UI Emoji), so TeamSpeak's own emoji are shown."));
    m_jumbo  = switchRow(group, i18n::t("Show messages of only emoji lar&ge"), i18n::t("Up to 27 emoji with nothing else in the message, like in Discord."),
                        1); // depends on the first one
    m_button = switchRow(group, i18n::t("Use TS Media's emoji picker for TeamSpeak's emoji &button"),
                         i18n::t("Off: TeamSpeak's own emoticon list. Ctrl+E opens TS Media's picker either way."));

    // 2.5 GIFs: the picker's GIFs tab, and its list managed without the picker (also with the emoji button off).
    QGroupBox* gifCard = card(i18n::t("GIFs"), this);
    m_remember      = switchRow(gifCard, i18n::t("Remember GI&Fs I send") /* "lar&ge" has G on this page */,
                           i18n::t("The emoji picker's GIFs tab keeps the GIF links you send from GIF sites, and copies of GIF files you upload, "
                                   "on this PC so you can send them again. Nothing of it is sent anywhere. Open the picker with Ctrl+E when "
                                   "the emoji button is off."));
    m_removeAll = new QPushButton(i18n::t("Remove all GIFs…"), gifCard);
    m_removeAll->setObjectName(QString::fromLatin1("tsmediaRemoveAllGifs"));
    m_removeAll->setAutoDefault(false);
    m_gifCount = fieldRow(gifCard, QString(), m_removeAll, QString());
    m_gifCount->setBuddy(nullptr); // a readout, not the button's label

    connect(m_hd, &QCheckBox::toggled, this, [this] {
        updateEnabled();
        emit changed();
    });
    connect(m_jumbo, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_button, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_remember, &QCheckBox::toggled, this, &SettingsSection::changed);
    connect(m_removeAll, &QPushButton::clicked, this, [this] { confirmRemoveAll(); });
    if (GifLibrary* gifs = GifLibrary::instance())
        connect(gifs, &GifLibrary::changed, this, [this] { updateGifs(); });
    updateGifs();
}

void EmojiSection::updateGifs()
{
    GifLibrary* gifs     = GifLibrary::instance();
    const int   count    = gifs ? gifs->count() : 0;
    const bool  anything = gifs && gifs->hasAnythingToRemove();
    if (count > 0) {
        m_gifCount->setText(i18n::t("GIFs: %1 · %2").arg(count).arg(formatSize(static_cast<quint64>(gifs->copyBytes()))));
    } else if (anything) {
        // Copies an unreadable list (moved aside) or a newer version's hidden entries still name: Remove all frees them.
        m_gifCount->setText(i18n::t("Leftover GIF copies: %1").arg(formatSize(static_cast<quint64>(gifs->leftoverBytes()))));
    } else {
        m_gifCount->setText(i18n::t("No GIFs saved"));
    }
    m_removeAll->setEnabled(anything);
}

// Remove all's question for count GIFs (none: the leftover copies only; one: no "1 GIFs").
QString EmojiSection::removeAllQuestion(int count)
{
    if (count == 0)
        return i18n::t("Remove the leftover GIF copies from this computer?");
    if (count == 1)
        return i18n::t("Remove the GIF from this computer?");
    return i18n::t("Remove all %1 GIFs from this computer?").arg(count);
}

// Not QMessageBox's static helpers (their buttons follow TeamSpeak's language) and open(), not exec(): the Clear cache
// pattern of the settings dialog.
void EmojiSection::confirmRemoveAll()
{
    GifLibrary* gifs = GifLibrary::instance();
    if (!gifs || !gifs->hasAnythingToRemove())
        return;
    auto* box = new QMessageBox(QMessageBox::Warning, window()->windowTitle(), removeAllQuestion(gifs->count()), QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setInformativeText(i18n::t("This can't be undone."));
    QPushButton* remove = box->addButton(GifPanel::removeButtonText(gifs->count()), QMessageBox::DestructiveRole); // 2.6: "Remove" for one
    QPushButton* cancel = box->addButton(i18n::t("Cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(cancel); // Enter must not delete anything
    box->setEscapeButton(cancel);
    connect(remove, &QPushButton::clicked, this, [this] {
        if (GifLibrary* library = GifLibrary::instance())
            library->clear();
        updateGifs();
    });
    box->open();
}

void EmojiSection::updateEnabled()
{
    setRowEnabled(m_hd, m_colour);
    setRowEnabled(m_jumbo, m_colour && m_hd->isChecked());
}

void EmojiSection::load(const Settings& s)
{
    const QSignalBlocker a(m_hd);
    const QSignalBlocker b(m_jumbo);
    const QSignalBlocker c(m_button);
    const QSignalBlocker d(m_remember);
    m_hd->setChecked(s.hdEmoji);
    m_jumbo->setChecked(s.jumboEmoji);
    m_button->setChecked(s.emojiButton);
    m_remember->setChecked(s.rememberGifs);
    updateEnabled();
    updateGifs();
}

void EmojiSection::store(Settings& target, const Settings& loaded) const
{
    take(target.hdEmoji, loaded.hdEmoji, m_hd->isChecked());
    take(target.jumboEmoji, loaded.jumboEmoji, m_jumbo->isChecked());
    take(target.emojiButton, loaded.emojiButton, m_button->isChecked());
    // 2.5 GIFs: take() writes it only when it was changed here, so the picker's own toggle is never written back.
    take(target.rememberGifs, loaded.rememberGifs, m_remember->isChecked());
}
