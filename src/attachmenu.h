#pragma once

// 2.3 attach: the entries of the chat input's attach menu (the paperclip left of "Enter Chat Message...",
// attachbutton.h). Features register their entry here and remove it again when they go (plugin shutdown):
// the menu (attachpopup.h) lists entries() each time it opens, sorted by order.
//
// Orders in use (gaps left for later entries):
//   10 "Photo or video"  ChatIntegration: the file chooser for pictures and videos, then the send window
//   20 "Document"        ChatIntegration: the file chooser for any file, then the send window
//   30 "Poll"            ChatPolls: the new-poll window (chatpolls.h)
//   40 "Live music"      the live-music feature
// The functions are called on the GUI thread only, while the entry is registered. They may hold code of
// this DLL: an entry must be removed before the plugin unloads (removeEntry in the owner's destructor).

#include <QIcon>
#include <QList>
#include <QString>

#include <functional>

class QWidget;

namespace attachmenu {

struct Entry {
    QString                       id;    // unique; a second addEntry with the same id replaces the first
    int                           order = 0;
    std::function<QString()>      text;    // "Photo or video" (no mnemonic; the menu has none)
    std::function<QIcon()>        icon;    // 24 x 24 logical px; empty: no icon
    std::function<bool()>         enabled; // empty: always enabled
    std::function<void(QWidget*)> trigger; // anchor: the paperclip button that opened the menu (may be nullptr)
};

void         addEntry(const Entry& entry);
void         removeEntry(const QString& id);
QList<Entry> entries(); // by order, then id

} // namespace attachmenu
