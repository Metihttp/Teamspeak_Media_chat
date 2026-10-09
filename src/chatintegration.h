#pragma once

#include <QHash>
#include <QPointer>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QVector>

#include "core.h"
#include "previewrenderer.h"

class InlineMediaController;
class MediaViewer;
class QMimeData;
class QTabBar;
class QTextBrowser;
class QTextDocument;
class QTimer;
class UploadToast;

// Hooks into the TeamSpeak chat widgets (QTextBrowser based):
//  * finds ts3file:// links in chat and inserts inline previews / players / file cards right below them
//  * hides the "plugin required" note that follows TS Media links (it is meant for clients without the plugin)
//  * forwards hover/clicks to InlineMediaController (GIFs, video controls) and opens MediaViewer
//  * right-click menu on previews (open, save, copy, show in folder, ...)
//  * lets users drop files on the chat or paste screenshots/copied files into the chat line to send
//    them (a paste asks first: the clipboard may hold something old the user did not mean to send)
class ChatIntegration : public QObject
{
    Q_OBJECT

  public:
    explicit ChatIntegration(Core* core, QObject* parent = nullptr);
    ~ChatIntegration() override;

    void start();

    // Chat target of the chat tab that is currently visible. A private chat whose partner cannot be
    // identified yields TextMessageTarget_CLIENT with clientId 0 (nothing can be sent there; never
    // the channel instead).
    ChatTarget currentTarget() const;
    QWidget*   mainWindow() const;

    void    pickAndSendFiles();
    void    refreshAll(); // after settings changes
    QString dumpWidgetTree() const;

    // Opens MediaViewer on key with every other previewable media of the same chat as a gallery.
    void openViewer(const QString& key);

    InlineMediaController* media() const { return m_media; }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- implementation (owned by chatintegration.cpp; may be reorganised freely) --------------
  private:
    struct PreviewPos {
        int     position = 0; // document position of the preview object
        QString key;
    };

    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        bool                    scanQueued     = false;
        bool                    relayoutQueued = false;
        int                     layoutWidth    = 0;     // maxWidth the previews were laid out for
        qreal                   layoutDpr      = 0.0;   // ... the device pixel ratio they were rendered at
        bool                    layoutDark     = false; // ... and the theme

        // Our preview objects in document order; rebuilt lazily after the document changed.
        bool                         positionsValid = false;
        QVector<PreviewPos>          previews;
        QHash<QString, QVector<int>> positionsByKey;
        QHash<QString, QSize>        formatSizes; // logical size each preview occupies in the layout
        QSet<QString>                resourced;   // keys with an image resource in this document
        QSet<QString>                stale;       // frames that changed while this chat was hidden

        // Test builds: throttled chat snapshots.
        qint64  lastSnapshotMs  = 0;
        bool    snapshotPending = false;
        int     snapshotCount   = 0;
        QString snapshotReason;
    };

    struct Hit {
        QString key;
        QRectF  rect; // preview rect in viewport coordinates
    };

    void discover();
    void attachBrowser(QTextBrowser* browser);
    void attachInput(QWidget* input);
    void watchDocument(QTextBrowser* browser, QTextDocument* document);
    void scheduleScan(QTextBrowser* browser);
    void scan(QTextBrowser* browser);
    void insertPreview(QTextBrowser* browser, int position, const QString& key);
    void refreshPreview(QTextBrowser* browser, const QString& key, bool pixelsOnly);
    QImage renderFor(QTextBrowser* browser, const QString& key, QSize* logicalSize);
    Hit    previewAt(QTextBrowser* browser, const QPoint& viewportPos) const;
    void   updateVisibleKeys();
    void   showContextMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos);

    void onEntryChanged(const QString& key);
    void onFrameChanged(const QString& key);
    void onUploadChanged(int id);
    void onOpenRequested(const QString& key);

    View*        viewFor(QTextBrowser* browser);
    void         ensurePositions(View& view);
    QRectF       previewRect(QTextBrowser* browser, int position, const QSizeF& size) const;
    bool         onScreen(QTextBrowser* browser, View& view, const QString& key);
    PreviewStyle styleFor(QTextBrowser* browser) const;
    int          previewAreaWidth(QTextBrowser* browser) const;
    void         activate(const Hit& hit, const QPointF& viewportPos, bool controlsVisible);
    void         openKey(const QString& key);
    void         copyImage(const QString& key);
    // actionable: a click there does something (pointing hand); otherwise the arrow.
    void         updateCursor(QTextBrowser* browser, bool overPreview, const QPoint& viewportPos, bool actionable = true);
    void         repaintPointerState(QTextBrowser* browser, const QString& key, bool includeVideos); // hover / pressed look
    QString      toolTipText(QTextBrowser* browser, const Hit& hit, const QPointF& viewportPos, QRect* area) const;
    void         scheduleVisibilityUpdate();
    void         scheduleRelayout(QTextBrowser* browser);
    bool         testShowsRawChat(const MediaLink& link) const;
    void         requestSnapshot(QTextBrowser* browser, const QString& reason);

    bool          acceptsDrop(const QMimeData* mime) const;
    void          sendMime(const QMimeData* mime, const ChatTarget& target);
    void          confirmPaste(QWidget* input, const QStringList& files, const QImage& image, const ChatTarget& target);
    bool          resolveTarget(QWidget* widget, ChatTarget* target) const; // false: unknown private chat partner
    bool          resolveCurrentTarget(ChatTarget* target, QWidget** source) const;
    void          warnNoRecipient(QWidget* widget) const;
    QString       describeTarget(const ChatTarget& target) const;
    QTabBar*      chatTabBarFor(QWidget* widget) const;
    QTextBrowser* visibleChatBrowser() const;
    QTextBrowser* browserForViewport(QObject* viewport) const;

    Core*                      m_core;
    InlineMediaController*     m_media         = nullptr;
    QTimer*                    m_discoverTimer = nullptr;
    QTimer*                    m_visibilityTimer = nullptr;
    QHash<QTextBrowser*, View> m_views;
    QList<QPointer<QWidget>>   m_inputs;
    QPointer<UploadToast>      m_toast;
    QString                    m_pressedKey;
    QString                    m_hoverKey;
    QPointer<QWidget>          m_cursorOwner;
    bool                       m_mutating = false;

    QPointer<QTextBrowser> m_lastBrowser;  // chat the user last interacted with (viewer gallery source)
    QPointer<QTextBrowser> m_pressBrowser;
    QRectF                 m_pressRect;
    bool                   m_pressControlsVisible = false; // video controls were shown when the button went down
    bool                   m_seeking          = false; // dragging on a video's seek bar
    qint64                 m_lastSeekMs       = 0;
    Qt::CursorShape        m_savedCursor      = Qt::ArrowCursor;
    bool                   m_visibilityQueued = false;
    bool                   m_testRawChat      = false; // TSMEDIA_TESTHOOKS "nohide"
    mutable int            m_scrollBarExtent  = 0;     // width a shown vertical scroll bar takes (measured)
    QPointer<QWidget>      m_pasteConfirm;             // open "send what was pasted?" prompt
    QPointer<MediaViewer>  m_viewer;                   // viewer opened from the chat (paused when an inline video starts)
};
