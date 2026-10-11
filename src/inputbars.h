#pragma once

// 2.3 live music: the bars above TeamSpeak's chat input, stacked without overlapping.
//
//   music bar    (in TeamSpeak's layout, at the top of the stack)
//   reply bar    (in TeamSpeak's layout, right above the input: chatreplies.cpp)
//   [chat input]
//
// Bars that stay a while go into the vertical layout that holds the input, so the chat above gets
// shorter instead of being covered; TeamSpeak's layout gets them back when they are deleted (Qt takes a
// deleted widget out of its layout). Brief overlays (the hold-to-record strip) float over the bottom of
// the chat, right above the input and whatever of ours is stacked on it (stackTop). Every bar of ours is
// a "tsmedia…" widget, which is how the stack is recognised (and how plugin shutdown finds them).

class QRect;
class QWidget;

namespace inputbars {

// Puts bar into the vertical layout that holds input (directly, or through rows around it), above the
// input and above TS Media's bars already there. False if there is no such layout: float it instead
// (placeOverlay).
bool insertAbove(QWidget* input, QWidget* bar);

// A floating bar as wide as input, right on top of the stack above it, in input's window (made its
// child if it isn't), raised over the chat. Again whenever the input moves or the stack changes.
void placeOverlay(QWidget* input, QWidget* bar, int height);

// The top edge, in owner's coordinates, of input together with the visible bars of ours stacked right
// on it (in TeamSpeak's layout or floating): where an overlay above the input goes. except: a bar that
// doesn't count (the overlay asking). topBar (2.3, may be null): the topmost bar's rect in owner's
// coordinates, empty when no bar is on the input (an overlay on the stack is as wide as it).
int stackTop(QWidget* input, QWidget* owner, const QWidget* except = nullptr, QRect* topBar = nullptr);

// Ours: a visible "tsmedia…" widget that isn't a window and isn't inside input.
bool isStackBar(const QWidget* widget, const QWidget* input);

} // namespace inputbars
