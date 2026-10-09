#pragma once

// 2.2 compose: the places where other features plug into the send window (ComposeDialog) without
// editing it. GUI thread only.

#include <functional>

#include "core.h" // ChatTarget

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
// PeerHub registers PresenceLine in start() and resets the factory (setPresenceLineFactory({})) in
// prepareShutdown and its destructor, before anything it uses goes; the send windows are closed by
// then. Called on the GUI thread.
using PresenceLineFactory = std::function<QWidget*(QWidget* parent, const ChatTarget& target)>;

void                setPresenceLineFactory(PresenceLineFactory factory);
PresenceLineFactory presenceLineFactory();
QWidget*            createPresenceLine(QWidget* parent, const ChatTarget& target); // nullptr: no factory, or it made none

// ---- albums (the album feature) ---------------------------------------------------------------------
//
// "Send as an album" is offered for 2 or more pictures and videos only while albums are enabled:
// albums::enabled() (albums.h), the one switch for sending, grouping and drawing albums.
bool albumsEnabled();

// Our own drags (files dragged out of a chat or the viewer) carry filedrag::mimeFormat(); the send
// window refuses them with filedrag::isOwn(), like the chat and its input do.

} // namespace compose
