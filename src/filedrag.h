#pragma once

// 2.2 drag-out: dragging a downloaded picture, video or file out of the chat or the viewer, and the
// "Copy file" alternative (context menu, Ctrl+Shift+C in the viewer) for keyboard and single-pointer
// use. What Explorer gets is Core::prepareExport(): a hard link with a clean name and the
// Mark-of-the-Web. Our own drags carry mimeFormat(), and every TS Media drop target refuses them,
// so a file dropped back on a chat is never uploaded again by accident.

#include <QMimeData>
#include <QPointF>
#include <QString>
#include <Qt>

class Core;
class QFont;
class QObject;

namespace filedrag {

// "application/x-tsmedia-key": the media key of a drag that started in TS Media. The one own-drag
// marker (decisions: compose, chat drop and drag-out share it). Built at run time: drag and clipboard
// data can outlive the DLL.
inline QString mimeFormat()
{
    return QString::fromLatin1("application/x-tsmedia-key");
}
// A drag that started in TS Media (refused by the chat, the chat input and the send window).
inline bool isOwn(const QMimeData* mime)
{
    return mime && mime->hasFormat(mimeFormat());
}

// The data of a drag: the file as a local URL (Windows gets CF_HDROP), plus mimeFormat() = key when
// own is set. Plain QMimeData with run-time strings only: the clipboard keeps it after the plugin is
// unloaded.
QMimeData* makeMime(const QString& path, const QString& key, bool own);

struct Result {
    Qt::DropAction action = Qt::IgnoreAction;
    QString        message; // when nothing was dragged: why, for a tooltip or a flash ("Still downloading (45%)…")
    bool           error = false;
};

// 2.2 spoiler: what a drag or "Copy file" of a covered spoiler says instead (start() and
// copyToClipboard() refuse those: Core::isSpoilerHidden).
QString spoilerRefusedText();

// Why the file of key can't be dragged or copied yet. A file that is not downloaded (Idle: held by
// the data saver, too large for automatic downloads, not fetched yet) starts downloading, exactly as
// a click on Download would. Empty when it is Ready.
QString notReadyText(Core* core, const QString& key, bool startDownload);

// Starts the drag (synchronously: Windows' drag loop runs inside, while the mouse button is still
// down). source owns the drag and must outlive the call. pressFraction: where in the preview the
// button went down (0..1), for the thumbnail's hot spot. Only CopyAction is offered, so Explorer
// never moves the cached file.
Result start(Core* core, const QString& key, QObject* source, const QPointF& pressFraction, qreal dpr, const QFont& font);

// "Copy file": the file on the clipboard, for Ctrl+V in Explorer (or in a chat input, which then asks
// whether to send it, as for any copied file: no own marker here). *feedback: the confirmation or why not.
bool copyToClipboard(Core* core, const QString& key, QString* feedback);

} // namespace filedrag
