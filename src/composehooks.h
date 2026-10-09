#pragma once

// 2.2: what other features plug into the send window (ComposeDialog). The protocol area registers the
// presence line here ("3 of 5 people here will see it in the chat"); the send window only places it.
// Shared by the compose and protocol branches: keep this header exactly as it is.

#include <functional>

#include "core.h" // ChatTarget

class QWidget;

namespace compose {
using PresenceLineFactory = std::function<QWidget*(QWidget*, const ChatTarget&)>;
void                setPresenceLineFactory(PresenceLineFactory);
PresenceLineFactory presenceLineFactory();
} // namespace compose
