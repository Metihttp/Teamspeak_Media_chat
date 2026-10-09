#pragma once

#include <QHash>
#include <QPointer>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QThreadPool>
#include <QVector>

#include "core.h"
#include "previewrenderer.h"

class ComposeDialog; // 2.2 compose
class ChatReactions; // 2.2 reactions
class InlineMediaController;
class MediaViewer;
class QDropEvent;
class QMenu;
class QMimeData;
class QTabBar;
class QTextBrowser;
class QTextCharFormat;
class QTextDocument;
class QTextEdit;
class QTimer;
class RevealFades;
class UploadToast;
class VoiceController; // 2.2 voice

// Hooks into the TeamSpeak chat widgets (QTextBrowser based):
//  * finds ts3file:// links in chat and inserts inline previews / players / file cards right below them
//  * hides the "plugin required" note that follows TS Media links (it is meant for clients without the plugin)
//  * forwards hover/clicks to InlineMediaController (GIFs, video controls) and opens MediaViewer
//  * right-click menu on previews (open, save, copy, show in folder, ...)
//  * lets users drop files on the chat or paste screenshots/copied files into the chat input to send
//    them (2.2: paste, drop and the file picker open the send window, ComposeDialog; a drop can send
//    right away instead, see Settings::dropOpensSendWindow; a drag shows where the drop will go)
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

    // 2.2 diagnostics: how many of TeamSpeak's chat views and input lines are hooked (counts only).
    int hookedChatViews() const { return m_views.size(); }
    int hookedInputs() const
    {
        int count = 0;
        for (const QPointer<QWidget>& input : m_inputs)
            count += input.isNull() ? 0 : 1; // isNull(): no complete QWidget needed here
        return count;
    }
    // ---- 2.2 voice: the recorder (voicecontroller.h), owned here so it goes before the players and Core
    VoiceController* voice() const { return m_voice; }
    // The visible chat as a voice message's destination, its description ("the channel “Lobby”") and
    // the chat input the recorder sits above. False (the chat already says why) when nothing can be
    // sent from there.
    bool voiceTarget(ChatTarget* target, QString* description, QWidget** anchor);
    // Pauses every inline player and the viewer.
    void pauseAllPlayback();
    // The destination as the recorder window names it ("the channel “Lobby”"); for the channel it
    // follows a channel switch (the message goes to the channel the user is in when it is sent).
    QString voiceTargetText(const ChatTarget& target) const { return describeTarget(target); }

  signals:
    // 2.2 voice: an inline player or the viewer started playing (the recorder pauses it again).
    void playbackStarted();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- implementation (owned by chatintegration.cpp; may be reorganised freely) --------------
  private:
    friend class ChatReactions; // 2.2 reactions: the reaction row under previews (chatreactions.cpp)

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
        QHash<QString, QStringList>  albumsByKey; // 2.2 album: key -> album grids (object ids) that show it

        // Test builds: throttled chat snapshots.
        qint64  lastSnapshotMs  = 0;
        bool    snapshotPending = false;
        int     snapshotCount   = 0;
        QString snapshotReason;
    };

    struct Hit {
        QString key;
        QRectF  rect; // preview rect in viewport coordinates
        // 2.2 album: the object under the pointer (a single preview: its key; an album: its object id).
        // On an album, key and rect are the tile's (key empty: an item that hasn't arrived), tile its
        // index (-1: a gap between tiles), overflow the "+N" of the last tile (0: none).
        QString object;
        int     tile     = -1;
        int     overflow = 0;
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
    // Hover look, video controls and cursor for what is under viewportPos. moved: the pointer moved
    // (that keeps video controls shown); otherwise the chat moved under a still pointer and only a
    // change of preview or control counts. *zone: the video control there.
    Hit    updateHoverAt(QTextBrowser* browser, const QPointF& viewportPos, bool moved, VideoZone* zone = nullptr);
    void   updateVisibleKeys();
    // 2.2 album: album is the grid's object id when key is one of its tiles (its reactions are the album's).
    void   showContextMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos, const QString& album = QString());

    void onEntryChanged(const QString& key);
    void onFrameChanged(const QString& key);
    void onUploadChanged(int id);
    void onCaptionSettled(int batch, bool posted); // 2.2 compose
    void onOpenRequested(const QString& key);

    View*        viewFor(QTextBrowser* browser);
    void         ensurePositions(View& view);
    QRectF       previewRect(QTextBrowser* browser, int position, const QSizeF& size) const;
    bool         onScreen(QTextBrowser* browser, View& view, const QString& key);
    PreviewStyle styleFor(QTextBrowser* browser) const;
    int          previewAreaWidth(QTextBrowser* browser) const;
    void         activate(const Hit& hit, const QPointF& viewportPos, bool controlsVisible);
    void         openKey(const QString& key);
    bool         copyImage(const QString& key, QString* feedback); // feedback: "Image copied" or why not
    // actionable: a click there does something (pointing hand); otherwise the arrow.
    void         updateCursor(QTextBrowser* browser, bool overPreview, const QPoint& viewportPos, bool actionable = true);
    void         repaintPointerState(QTextBrowser* browser, const QString& key, bool includeVideos); // hover / pressed look
    QString      toolTipText(QTextBrowser* browser, const Hit& hit, const QPointF& viewportPos, QRect* area) const;
    void         scheduleVisibilityUpdate();
    void         scheduleRelayout(QTextBrowser* browser);
    bool         testShowsRawChat(const MediaLink& link) const;
    void         requestSnapshot(QTextBrowser* browser, const QString& reason);

    // 2.2 spoiler (chatintegration_spoiler.cpp): a click on a covered preview only reveals it (with a
    // short crossfade); its menu offers "Reveal spoiler" and leaves out what would show the content.
    void    revealSpoiler(const QString& key);
    void    showSpoilerMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos, const QString& album = QString());
    void    addHideSpoilerAction(QMenu* menu, const QString& key); // revealed spoilers: cover it again
    qreal   spoilerCoverOpacity(const QString& key) const;          // PreviewStyle::concealOpacity
    QString spoilerToolTip() const;

    // Why nothing can be sent from a chat right now (checked before a picker, paste prompt or drop).
    enum class SendBlock { None, NoRecipient, NotConnected, Password };

    bool          acceptsDrop(const QMimeData* mime) const;
    void          sendMime(const QMimeData* mime, const ChatTarget& target);
    // 2.2 compose: the send window for files or a picture (raised, and given them when it is already
    // open for the same chat). caption: text from the chat input; input is cleared after a send when it
    // still holds exactly inputText.
    void          openCompose(QWidget* source, const QStringList& files, const QImage& image, const ChatTarget& target,
                              const QString& caption = {}, QTextEdit* input = nullptr, const QString& inputText = {});
    bool          dropSendsNow(const QDropEvent* drop) const; // 2.2 compose: the setting, inverted by Ctrl
    bool          resolveTarget(QWidget* widget, ChatTarget* target) const; // false: unknown private chat partner
    bool          resolveCurrentTarget(ChatTarget* target, QWidget** source) const;
    SendBlock     checkSend(QWidget* widget, ChatTarget* target) const; // resolveTarget, connection, channel password
    QString       blockText(SendBlock block) const;
    void          warnCantSend(QWidget* widget, SendBlock block) const; // tooltip at the widget + chat line
    QString       describeTarget(const ChatTarget& target) const;
    QTextBrowser* chatBrowserFor(QWidget* widget) const; // the chat a drop on widget goes to
    void          showDropOverlay(QWidget* widget, const QMimeData* mime);
    void          hideDropOverlay();
    void          showFeedback(QWidget* widget, const QString& text, bool error) const; // brief tooltip at the pointer
    QTabBar*      chatTabBarFor(QWidget* widget) const;
    QTextBrowser* visibleChatBrowser() const;
    QTextBrowser* browserForViewport(QObject* viewport) const;

    // ---- 2.2 album grid (chatalbums.cpp) ---------------------------------------------------------
    // An album's items show as one grid (an image object named "tsmedia:" + albums::objectId) right
    // after the last of its links in its message. The other item links of that message are collapsed
    // into one zero-width character that keeps the link (its text is kept in the format and given back
    // on restore), and later messages of an album (the rare album sent as several messages) are hidden
    // with QTextBlock::setVisible. Both are tagged, so they are found and undone by property.
    struct AlbumPlan {
        struct Collapse {
            int from  = 0; // the line breaks in front of the link start here (end of the previous item)
            int start = 0; // the link
            int end   = 0;
        };
        struct Object {
            QString           id;        // albums::objectId
            int               block = 0; // block number of the message it goes into
            int               after = 0; // document position it goes after (end of its last item's link there)
            QVector<Collapse> collapse;  // the message's other item links
        };
        bool                active = false; // album items in the chat, or marks of ours to undo
        QSet<int>           members;        // start positions of album item links: no preview of their own
        QHash<int, QSet<QString>> memberKeys; // block number -> its album items' keys: no link to them there gets a preview (albums::memberKeys)
        QVector<Object>     objects;
        QSet<int>           hide;           // block numbers of messages to hide
        QVector<int>        restore;        // collapsed links to give back
        QVector<int>        staleSingles;   // single previews of album items (from before they were grouped)
        // What the document holds now.
        QHash<int, QString> existing; // position -> album object id
        QVector<int>        markers;  // positions of collapsed links
        QSet<int>           hidden;   // block numbers we hid (tagged and not visible)
        QSet<int>           tagged;   // block numbers with our tag; a message appended after a hidden one can inherit it
    };
    AlbumPlan   planAlbums(QTextBrowser* browser) const;
    void        applyAlbums(QTextBrowser* browser, const AlbumPlan& plan);
    void        restoreAlbums(QTextBrowser* browser); // shows hidden messages and gives collapsed links back
    void        indexAlbums(View& view) const;        // View::albumsByKey from positionsByKey
    void        refreshAlbumsOf(QTextBrowser* browser, const QString& key, bool pixelsOnly);
    void        scheduleAlbumRefresh(QTextBrowser* browser, const QString& id);
    void        flushAlbumRefresh();
    QImage      renderAlbumFor(QTextBrowser* browser, const QString& id, QSize* logicalSize);
    Hit         albumHit(QTextBrowser* browser, const QString& id, const QRectF& rect, const QPointF& viewportPos) const;
    void        albumVisibleKeys(QTextBrowser* browser, const QString& id, const QRectF& rect, const QRectF& viewport, QSet<QString>* keys) const;
    QString     albumToolTip(const Hit& hit, QRect* area) const;
    void        activateAlbumTile(const Hit& hit);
    bool        shownAlone(const QString& key) const; // key has a single preview of its own in some chat
    static QStringList albumKeysOf(const QString& id);
    // chatintegration.cpp's document helpers, for chatalbums.cpp.
    static QString resourceName(const QString& id);
    static QString objectIdOf(const QTextCharFormat& format);
    static bool    isOwnSeparator(QTextDocument* document, int position);
    static bool    isFileAnchor(const QTextCharFormat& format);
    static bool    atBottom(QTextBrowser* browser);

    Core*                      m_core;
    InlineMediaController*     m_media         = nullptr;
    VoiceController*           m_voice         = nullptr; // 2.2 voice
    QTimer*                    m_discoverTimer = nullptr;
    QTimer*                    m_visibilityTimer = nullptr;
    QHash<QTextBrowser*, View> m_views;
    QList<QPointer<QWidget>>   m_inputs;
    QPointer<UploadToast>      m_toast;
    QString                    m_pressedKey;
    QString                    m_hoverKey;
    QPointer<QTextBrowser>     m_hoverBrowser; // the chat m_hoverKey is hovered in (the same file can be in several)
    VideoZone                  m_hoverZone = VideoZone::None; // last passed to InlineMediaController::hover
    QPointer<QWidget>          m_cursorOwner;
    bool                       m_mutating = false;

    QPointer<QTextBrowser> m_lastBrowser;  // chat the user last interacted with (viewer gallery source)
    QPointer<QTextBrowser> m_pressBrowser;
    QRectF                 m_pressRect;
    QPoint                 m_pressPos; // 2.2 drag-out: where the left button went down (viewport coordinates)
    bool                   m_pressControlsVisible = false; // video controls were shown when the button went down
    bool                   m_seeking          = false; // dragging on a video's seek bar
    qint64                 m_lastSeekMs       = 0;
    Qt::CursorShape        m_savedCursor      = Qt::ArrowCursor;
    bool                   m_visibilityQueued = false;
    bool                   m_testRawChat      = false; // TSMEDIA_TESTHOOKS "nohide"
    mutable int            m_scrollBarExtent  = 0;     // width a shown vertical scroll bar takes (measured)
    QPointer<ComposeDialog> m_compose;                 // 2.2 compose: the open send window
    // 2.2 compose: chat input text that went out as a send's caption, cleared once Core posted it
    // (Core::captionSettled; batch id -> the input and its text then).
    struct CaptionClear {
        QPointer<QObject> input; // the chat input (a QTextEdit)
        QString           text;
    };
    QHash<int, CaptionClear> m_captionClears;
    QPointer<MediaViewer>  m_viewer;                   // viewer opened from the chat (paused when an inline video starts)
    QPointer<QWidget>      m_dropOverlay;              // "Drop to send" over the chat during a drag
    SendBlock              m_dropBlock = SendBlock::None; // checked when the drag entered a widget
    ChatTarget             m_dropTarget;
    bool                   m_dropNow = false;          // 2.2 compose: the overlay says the drop sends right away
    RevealFades*           m_reveals = nullptr;        // 2.2 spoiler: reveal crossfades (a child: stops with us)

    // 2.2 album: pointer state per tile (a single preview's object is its key, its tile -1)
    QString                                 m_hoverObject;
    int                                     m_hoverTile   = -1;
    QPointer<QTextBrowser>                  m_hoverObjectIn; // the chat m_hoverObject is hovered in
    QString                                 m_pressedObject;
    int                                     m_pressedTile = -1;
    QTimer*                                 m_albumTimer  = nullptr; // redraws albums with new GIF frames, ~30 per second
    QHash<QTextBrowser*, QSet<QString>>     m_albumDirty;
    ChatReactions*                          m_reactions = nullptr; // 2.2 reactions
};
