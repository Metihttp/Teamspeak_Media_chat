#pragma once

// 2.2 emoji: how an HD emoji sits in TeamSpeak's chat document. ChatEmoji (chatemoji.h) replaces an
// emoji (or one of TeamSpeak's emoticon pictures) by one picture "tsmemoji:<code>/<px>" whose format
// keeps what was there. Shared with the reply code (replydoc.h), which reads messages and must see the
// text the pictures stand for, and keeps a quote line it takes out of the document as it was received.
// Header only, QtGui.

#include <QString>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QVariant>

namespace emojiformat {

// Properties of the picture's format (replydoc's reply line uses others: 0x7470 and up).
constexpr int kOriginalText   = QTextFormat::UserProperty + 0x7460; // QString: the emoji's text
constexpr int kOriginalFormat = QTextFormat::UserProperty + 0x7461; // QTextFormat: the character's format before
constexpr int kEmoticonName   = QTextFormat::UserProperty + 0x7462; // QString: the TeamSpeak emoticon picture it replaced
constexpr int kJumbo          = QTextFormat::UserProperty + 0x7463; // bool: a large one (a message of only emoji)

// One of ChatEmoji's pictures (named "tsmemoji:<code>/<px>").
inline bool isHd(const QTextCharFormat& format)
{
    return format.isImageFormat() && (format.hasProperty(kOriginalText) || format.hasProperty(kEmoticonName))
           && format.toImageFormat().name().startsWith(QLatin1String("tsmemoji:"));
}

// The emoji's text; empty for a picture that replaced one of TeamSpeak's emoticons.
inline QString originalText(const QTextCharFormat& format)
{
    return format.stringProperty(kOriginalText);
}

// The picture name of the TeamSpeak emoticon it replaced ("emoticons:smile.svg"); empty otherwise.
inline QString emoticonName(const QTextCharFormat& format)
{
    return format.stringProperty(kEmoticonName);
}

// The format the character had before (TeamSpeak's emoticon: its image format).
inline QTextCharFormat originalFormat(const QTextCharFormat& format)
{
    return qvariant_cast<QTextFormat>(format.property(kOriginalFormat)).toCharFormat();
}

} // namespace emojiformat
