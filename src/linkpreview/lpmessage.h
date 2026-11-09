#pragma once

// 2.4 link previews, the sender's side: what its plugin reads of a chat message it has just sent, from the
// raw BBCode TeamSpeak hands to onTextMessageEvent (its own messages come back there). Pure QtCore.
//
//  * The links TeamSpeak shows: [URL]x[/URL], [URL=x]label[/URL] and bare web addresses. An opening tag
//    escaped with backslashes ("\[URL]", how captions and quotes are written) is plain text, as in the chat.
//  * The text TeamSpeak shows, for the message's identity (textreact.h): BBCode tags out, a link's label
//    in place of the tag, escaped brackets as brackets, and a reply's quote line (replies.h) left out, as
//    every receiver reads it from its chat document (textreactdoc::identify). Only letters, digits and
//    symbols count for the hash, so spacing and punctuation never make a difference.
//  * Messages with a TeamSpeak file link are TS Media's own media messages: they have no text identity
//    (textreactdoc: "media"), so they get no link preview.

#include <QList>
#include <QString>
#include <QUrl>

namespace linkpreview::message {

constexpr int kMaxLinks = 3; // previewable links per message (the first ones)

// The text TeamSpeak shows for bbcode (without a reply's quote line).
QString visibleText(const QString& bbcode);
// textreact::contentHash(senderUid, visibleText(bbcode)).
QString identityHash(const QString& senderUid, const QString& bbcode);
// Every http(s) link TeamSpeak shows in bbcode (outside a reply's quote line), in order, at most max. A
// [URL] of "www...." without a scheme counts as http:// (as the chat links it).
QList<QUrl> webLinks(const QString& bbcode, int max = 32);
// A TS Media media message (it has a ts3file link).
bool isMediaMessage(const QString& bbcode);

} // namespace linkpreview::message
