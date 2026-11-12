#pragma once

// 2.5: one safe way to decode a picture from somewhere else (a website, another client's file). Qt's image readers
// decide the format from a file's content by default, so a "preview.jpg" holding SVG would be read by the SVG
// plugin (which may fetch addresses named inside it) and an XPM by the XPM reader. Here the format is decided by
// the file's first bytes against a short list, and forced: no other reader ever sees the file. WebP files are
// checked first (content::webpVerdict): lossless WebP (CVE-2023-4863 in the libwebp of TeamSpeak's Qt 5.15.2) and
// malformed containers are refused.
//
//  * Website: png, jpeg, gif, webp;
//  * Peer (another client's file): the same and bmp;
//  * Local (a file the user picked): the same and bmp.

#include <QByteArray>
#include <QString>

#include <memory>

class QImageReader;
class QMovie;
class QObject;

namespace safeimage {

enum class Source { Website, Peer, Local };

// The format of a file from its first bytes ("png", "jpeg", "gif", "webp", "bmp"), or empty when it isn't one the
// plugin decodes for that source, or a WebP that is lossless or malformed.
QByteArray formatOf(const QString& path, Source source);
// A reader for path with that format forced (no format detection by content or name). A null reader (format()
// empty, canRead() false) when refused.
std::unique_ptr<QImageReader> reader(const QString& path, Source source);
// A QMovie for an animation ("gif" or "webp"), the format forced; nullptr when refused.
QMovie* movie(const QString& path, Source source, QObject* parent);

} // namespace safeimage
