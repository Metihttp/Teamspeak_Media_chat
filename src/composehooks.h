#pragma once

// 2.2 compose: the places where other features plug into the send window (ComposeDialog) without
// editing it. GUI thread only.

#include <QString>

#include <functional>

#include "core.h" // ChatTarget

class QMimeData;
class QWidget;

namespace compose {

// ---- "who will see it" line (the plugin-protocol feature) ----------------------------------------
//
// Builds the presence line shown under the send window's header for target ("3 of 5 people here will
// see it in the chat. The others get a download link."). The widget belongs to parent (the send window)
// and keeps itself up to date while it lives; it is created once per window, and hidden while the
// window shows why nothing can be sent (not connected, the private chat partner left). Returning
// nullptr shows nothing. Without a factory the window shows no presence line at all.
//
// The function is called on the GUI thread. Reset it (setPresenceLineFactory({})) at shutdown before
// deleting anything it uses; the send windows are always closed before that.
using PresenceLineFactory = std::function<QWidget*(QWidget* parent, const ChatTarget& target)>;

void     setPresenceLineFactory(PresenceLineFactory factory);
QWidget* createPresenceLine(QWidget* parent, const ChatTarget& target); // nullptr: no factory, or it made none

// ---- albums (the album feature) ---------------------------------------------------------------------
//
// "Send as an album" is offered for 2 or more pictures and videos only while albums are enabled. Off
// until the album feature is ready: build with TSMEDIA_ALBUMS defined, or call setAlbumsEnabled(true)
// at start-up. While it is off the window never asks Core for an album.
void setAlbumsEnabled(bool enabled);
bool albumsEnabled();

// ---- our own drags ------------------------------------------------------------------------------------
//
// "application/x-tsmedia-key": the marker on drags that started in TS Media (the drag-out feature's
// filedrag::mimeFormat(); TODO use filedrag::isOwn once both are merged). The chat and the send window
// never take such a drop, so a file dragged out of the chat can't be uploaded again by accident.
QString ownDragMimeFormat();
bool    isOwnDrag(const QMimeData* mime);

} // namespace compose
