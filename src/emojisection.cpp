#include "emojisection.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "emojirender.h"
#include "giflibrary.h" // 2.5 GIFs
#include "i18n.h"

EmojiSection::EmojiSection(QWidget* parent)
    : SettingsSection(parent)
{
    setObjectName(QString::fromLatin1("tsmediaEmojiSection"));
    m_colour    = emoji::hasColor();
    auto* group = new QGroupBox(i18n::t("Emoji"), this);

    m_hd = new QCheckBox(i18n::t("Show emoji in high &quality in the chat"), group);
    QLabel* hdHint =
        hint(m_colour ? i18n::t("Emoji and TeamSpeak's smileys like :) show as sharp, colorful pictures. Everyone with TS Media sees them like this; "
                                "people without it see TeamSpeak's own.")
                      : i18n::t("This PC has no color emoji font (Segoe UI Emoji), so TeamSpeak's own emoji are shown."),
             group);
    m_hd->setAccessibleDescription(hdHint->text());

    m_jumbo = new QCheckBox(i18n::t("Show messages of only emoji lar&ge"), group);
    QLabel* jumboHint = hint(i18n::t("Up to 27 emoji with nothing else in the message, like in Discord."), group);
    m_jumbo->setAccessibleDescription(jumboHint->text());

    m_button = new QCheckBox(i18n::t("Use TS Media's emoji picker for TeamSpeak's emoji &button"), group);
    QLabel* buttonHint = hint(i18n::t("Off: TeamSpeak's own emoticon list. Ctrl+E opens TS Media's picker either way."), group);
    m_button->setAccessibleDescription(buttonHint->text());

    // 2.5 GIFs: the picker's GIFs tab, and its list managed without the picker (also with the emoji button off).
    m_remember = new QCheckBox(i18n::t("Remember GI&Fs I send") /* "lar&ge" has G on this tab */, group);
    QLabel* rememberHint = hint(i18n::t("The emoji picker's GIFs tab keeps the GIF links you send from GIF sites, and copies of GIF files you upload, "
                                        "on this PC so you can send them again. Nothing of it is sent anywhere. Open the picker with Ctrl+E when "
                                        "the emoji button is off."),
                                group);
    m_remember->setAccessibleDescription(rememberHint->text());
    m_gifCount  = new QLabel(group);
    m_removeAll = new QPushButton(i18n::t("Remove all GIFs…"), group);
    m_removeAll->setObjectName(QString::fromLatin1("tsmediaRemoveAllGifs"));
    m_removeAll->setAutoDefault(false);
    auto* gifRow = new QHBoxLayout;
    gifRow->setContentsMargins(0, 0, 0, 0);
    gifRow->addWidget(m_gifCount, 1);
    gifRow->addWidget(m_removeAll);

    auto* rows = form(group);
    rows->addRow(m_hd);
    rows->addRow(hdHint);
    rows->addRow(m_jumbo);
    rows->addRow(jumboHint);
    rows->addRow(m_button);
    rows->addRow(buttonHint);
    rows->addRow(m_remember);
    rows->addRow(rememberHint);
    rows->addRow(gifRow);
    addIndented(rememberHint);
    addIndented(hdHint);
    addIndented(m_jumbo); // depends on the first one
    addIndented(jumboHint);
    addIndented(buttonHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);

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
    QPushButton* remove = box->addButton(i18n::t("Remove all"), QMessageBox::DestructiveRole);
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
    m_hd->setEnabled(m_colour);
    m_jumbo->setEnabled(m_colour && m_hd->isChecked());
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
