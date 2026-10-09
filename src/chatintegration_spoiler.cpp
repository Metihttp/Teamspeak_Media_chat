// 2.2 spoiler: how ChatIntegration treats covered previews (the rest of the class is in
// chatintegration.cpp). A hidden spoiler (Core::isSpoilerHidden) is drawn under its blurred cover, has
// no video controls, never animates, and its first click only reveals it; the tooltip and the menu
// never show its name or anything of its content.

#include "chatintegration.h"

#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QPointer>
#include <QTextBrowser>

#include "i18n.h"
#include "settings.h"
#include "spoiler.h"
#include "uiutil.h"

qreal ChatIntegration::spoilerCoverOpacity(const QString& key) const
{
    if (m_core->isSpoilerHidden(key))
        return 1.0;
    return m_reveals ? m_reveals->coverOpacity(key) : 0.0;
}

QString ChatIntegration::spoilerToolTip() const
{
    return i18n::t("Spoiler · Click to reveal");
}

void ChatIntegration::revealSpoiler(const QString& key)
{
    if (!m_core->isSpoilerHidden(key))
        return;
    // Before the reveal is announced (entryChanged): the redraw it causes still shows the whole cover,
    // which then fades. Instant when Windows animations are off.
    if (m_reveals)
        m_reveals->start(key, ui::animationsEnabled());
    m_core->setSpoilerRevealed(key, true);
}

// The menu of a covered preview: what a click does first (in bold), then only what doesn't show the
// content here (Open shows the viewer's cover; nothing plays, no Copy image, no default app).
void ChatIntegration::showSpoilerMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return;
    const bool ready = e->state == MediaState::Ready;
    const bool gone  = !isPreviewActionable(*e); // deleted from the server, password-protected channel

    auto* menu = new QMenu(browser);
    menu->setObjectName(QString::fromLatin1("tsmediaPreviewMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    QPointer<QTextBrowser> guard(browser);
    auto viewport = [guard]() -> QWidget* { return guard ? guard->viewport() : nullptr; };

    QAction* reveal = menu->addAction(i18n::t("&Reveal spoiler"), this, [this, key] { revealSpoiler(key); });
    menu->addSeparator();
    if (!gone)
        menu->addAction(i18n::t("&Open"), this, [this, key] { openKey(key); });
    if (ready)
        menu->addAction(i18n::t("Show in &folder"), this, [this, key] { m_core->revealInFolder(key); });
    menu->addSeparator();
    if (ready) {
        menu->addAction(i18n::t("&Save as…"), this, [this, key, guard, viewport] {
            const QString saved = m_core->saveAs(key, guard ? guard->window() : mainWindow());
            if (!saved.isEmpty())
                showFeedback(viewport(), ui::savedToText(saved), false);
        });
    }
    menu->addAction(i18n::t("Copy &link"), this, [this, key, viewport] {
        if (const MediaEntry* entry = m_core->entry(key)) {
            QGuiApplication::clipboard()->setText(entry->link.toUrl());
            showFeedback(viewport(), i18n::t("Link copied"), false);
        }
    });
    menu->setDefaultAction(reveal);
    menu->popup(globalPos);
}

// A revealed spoiler can be covered again (not while every spoiler is shown by the setting).
void ChatIntegration::addHideSpoilerAction(QMenu* menu, const QString& key)
{
    if (!m_core->isSpoiler(key) || m_core->isSpoilerHidden(key) || Settings::instance().revealSpoilers)
        return;
    menu->addSeparator();
    menu->addAction(i18n::t("&Hide spoiler"), this, [this, key] {
        if (m_reveals)
            m_reveals->cancel(key);
        // A video playing here stops (InlineMediaController); the viewer covers it too (entryChanged).
        m_core->setSpoilerRevealed(key, false);
    });
}
