#include "filedrag.h"

#include <QClipboard>
#include <QDrag>
#include <QFont>
#include <QGuiApplication>
#include <QMimeData>
#include <QPixmap>
#include <QPointer>
#include <QUrl>

#include "core.h"
#include "dragpixmap.h"
#include "i18n.h"

namespace filedrag {

namespace {

constexpr int kThumbnailBox = 160; // logical pixels, see renderDragPicture

// Windows' "Preferred DropEffect" clipboard format: Explorer pastes a copy (never moves the file).
QString preferredDropEffectFormat()
{
    return QString::fromLatin1("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
}

} // namespace

QMimeData* makeMime(const QString& path, const QString& key, bool own)
{
    // Not a subclass: the clipboard may keep this object after the plugin DLL is gone.
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    if (own)
        mime->setData(mimeFormat(), key.toLatin1());
    return mime;
}

QString notReadyText(Core* core, const QString& key, bool startDownload)
{
    const MediaEntry* e = core ? core->entry(key) : nullptr;
    if (!e)
        return i18n::t("Not downloaded yet");
    if (e->state == MediaState::Idle) {
        if (!startDownload)
            return i18n::t("Not downloaded yet");
        // Exactly what Download does; data saver and the automatic-download limits don't apply to it.
        core->download(key, false);
        e = core->entry(key);
        if (!e)
            return {};
    }
    switch (e->state) {
    case MediaState::Ready:
        return {};
    case MediaState::Idle:
        return i18n::t("Not downloaded yet");
    case MediaState::Queued:
        return startDownload ? i18n::t("Waiting to download. Drag it again when it's ready.") : i18n::t("Waiting to download…");
    case MediaState::Downloading: {
        const int percent = qRound(qBound(0.0, e->progress, 1.0) * 100.0);
        if (!startDownload)
            return i18n::t("Still downloading (%1%)").arg(percent);
        if (percent <= 0)
            return i18n::t("Downloading… Drag it again when it's ready.");
        return i18n::t("Still downloading (%1%). Drag it again when it's ready.").arg(percent);
    }
    case MediaState::Failed:
        return downloadErrorTitle(e->error);
    }
    return {};
}

Result start(Core* core, const QString& key, QObject* source, const QPointF& pressFraction, qreal dpr, const QFont& font)
{
    Result result;
    if (!core || !source || !core->entry(key))
        return result;
    result.message = notReadyText(core, key, true);
    if (!result.message.isEmpty()) {
        const MediaEntry* e = core->entry(key);
        result.error        = e && e->state == MediaState::Failed;
        return result;
    }

    // Everything needed is copied now: the drag runs a nested event loop in which chat updates and
    // transfers go on (the entry may change or go away meanwhile).
    const MediaEntry entry   = *core->entry(key);
    const bool       media   = isPreviewableImage(entry.kind) || entry.kind == MediaKind::Video;
    const bool       program = !media && core->isUnsafeToOpen(key);
    DragImage        image;
    if (media) {
        const QSize      pixels(qRound(kThumbnailBox * dpr), qRound(kThumbnailBox * dpr));
        const MediaStill still = core->still(key, pixels);
        if (still.source != MediaStill::None && !still.image.isNull())
            image = renderDragPicture(still.image, dpr, pressFraction);
    }
    if (image.image.isNull())
        image = renderDragCard(entry, program, font, dpr);

    const QString path = core->prepareExport(key);
    if (path.isEmpty()) {
        result.message = i18n::t("Couldn't prepare the file for dragging. Use Save as instead.");
        result.error   = true;
        return result;
    }

    const QPointer<Core> guard(core);
    core->setInUse(key, true);
    auto* drag = new QDrag(source); // deleted by Qt once the drag is over (or with its source)
    drag->setMimeData(makeMime(path, key, true));
    drag->setPixmap(QPixmap::fromImage(image.image));
    drag->setHotSpot(image.hotSpot);
    result.action = drag->exec(Qt::CopyAction, Qt::CopyAction);
    if (guard)
        guard->setInUse(key, false);
    return result;
}

bool copyToClipboard(Core* core, const QString& key, QString* feedback)
{
    const QString notReady = notReadyText(core, key, false);
    if (!notReady.isEmpty()) {
        *feedback = notReady;
        return false;
    }
    const QString path = core->prepareExport(key);
    if (path.isEmpty()) {
        *feedback = i18n::t("Couldn't copy the file. Use Save as… instead.");
        return false;
    }
    QMimeData* mime = makeMime(path, key, false);
    mime->setData(preferredDropEffectFormat(), QByteArray("\x01\x00\x00\x00", 4)); // DROPEFFECT_COPY
    QGuiApplication::clipboard()->setMimeData(mime);
    *feedback = i18n::t("File copied. Paste it into a folder with Ctrl+V.");
    return true;
}

} // namespace filedrag
