#pragma once

#include <QHash>
#include <QPointer>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QThreadPool>
#include <QVector>

#include <functional>

#include "core.h"
#include "previewrenderer.h"
#include "previewscale.h" // 2.5 zoom

class ComposeDialog; // 2.2 compose
class ChatReactions; // 2.2 reactions
class ChatTextReactions; // 2.3 text reactions
class ChatReplies;   // 2.2 reply
class ChatEmoji;     // 2.2 emoji
class ChatLayout;    // chat redesign
class ChatPolls;     // 2.3 poll
class ChatScale;     // 2.5 zoom: one per chat (chatscale.h)
class ChatLinkPreviews; // 2.4 link previews
class GifLibrary;       // 2.5 GIFs
class QUrl;
namespace linkpreview {
struct Target;
}
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
    // 2.3 attach: the attach menu's "Photo or video" (media: pictures and videos) and "Document" (any file):
    // the file chooser for the chat of source (the chat input the paperclip is in), then the send window.
    void    pickFiles(QWidget* source, bool media);
    ChatPolls* polls() const { return m_polls; } // 2.3 poll
    // 2.4 link previews: the cards under web links (integration: linkPreviews()->setHub(<the link-preview hub>, <the peer directory>)).
    ChatLinkPreviews* linkPreviews() const { return m_links; }
    // A Core key shown as a web link's media (its message has text reactions, the file has none of its own).
    bool isLinkPreviewKey(const QString& key) const;
    void    refreshAll(); // after settings changes
    QString dumpWidgetTree() const;

    // Opens MediaViewer on key with every other previewable media of the same chat as a gallery.
    void openViewer(const QString& key);

    InlineMediaController* media() const { return m_media; }
    // Chat redesign: the modules on the chat (tests, tools and the live-test driver).
    ChatLayout*  layout() const { return m_layout; }
    ChatEmoji*   emoji() const { return m_emoji; }
    ChatReplies* replies() const { return m_replies; }

    // 2.2 reply: the "Reply to the last message" hotkey (chatreplies.h).
    void replyToLatest();

    // 2.3.4: links in TS Media's own chat lines (ts3::printInfoLink) that the plugin handles itself (the
    // newer-version hint, "#tsmupd-" in the address). handler(href, false) at the press: "is it ours";
    // handler(href, true) at the release on the same link: run it. Empty: none (TeamSpeak opens them).
    void setOwnLinkHandler(std::function<bool(const QString& href, bool activate)> handler);

    // 2.5 GIFs (plugin.cpp, after start()): the library behind the emoji picker's GIFs tab: the chat input's picker gets
    // the tab (its host resolves the input's chat at opening and at each click), and the media menu gets Save to GIFs.
    // nullptr: neither.
    void setGifs(GifLibrary* gifs);
    // 2.5 GIFs: the current server tab changed (plugin.cpp): a picker with the GIFs tab closes.
    void closeGifPicker();

    // 2.5 zoom: the scale the chat's previews are laid out for now (previewscale.h; the applied one, set by its
    // ChatScale pass), and the one its text size, column, display and the Preview size setting ask for.
    PreviewScale scaleOf(QTextBrowser* browser) const;
    PreviewScale computeScaleFor(QTextBrowser* browser) const;
    // 2.5 zoom: the chat's coordinator (re-layout without jumps, the picture budget); nullptr: not attached.
    ChatScale* chatScale(QTextBrowser* browser) const;
    // 2.5 zoom (1.12): a reply line's or a text reaction row's picture (or its blank stand-in) into doc through the
    // chat's picture budget when doc is an attached chat's document (else straight into doc); drop: it goes (memory and
    // books). module: ChatScale::Replies / TextReactions.
    void putChromePicture(QTextDocument* doc, const QString& name, const QPixmap& pixmap, int module);
    void dropChromePicture(QTextDocument* doc, const QString& name);
    // The visible chat's applied scale and text size in px (the diagnostic info); false: no chat is shown.
    bool       visibleChatScale(PreviewScale* scale, int* chatPx) const;

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
    // 2.2.1 mic: why the visible chat can't take a voice message now (the mic button's tool tip), or
    // empty. Nothing is shown or printed.
    QString voiceBlockReason() const;

  signals:
    // 2.2 voice: an inline player or the viewer started playing (the recorder pauses it again).
    void playbackStarted();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- implementation (owned by chatintegration.cpp; may be reorganised freely) --------------
  private:
    friend class ChatReactions; // 2.2 reactions: the reaction row under previews (chatreactions.cpp)
    friend class ChatTextReactions; // 2.3 text reactions: the reaction row under messages without media (chattextreactions.cpp)
    friend class ChatReplies;   // 2.2 reply: reply lines, the reply bar and menu items (chatreplies.cpp)
    friend class ChatLayout;    // chat redesign: the chat layout (chatlayout.cpp)
    friend class ChatPolls;     // 2.3 poll: poll cards in the chat (chatpolls.cpp)
    friend class ChatLinkPreviews; // 2.4 link previews: the cards and media under web links (chatlinkpreviews.cpp)
#ifdef TSMEDIA_TESTHOOKS
    friend class SelfTest; // test builds: the scripted live-test driver (selftest.cpp)
#endif

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
        QString                 layoutBubbles;          // 2.3: ... and the bubbles they sit on (surfacesKey)
        // 2.5 zoom: the scale every preview object of this chat is laid out for (formats, slots, cards), and the
        // coordinator that changes it in one pass (a child of ChatIntegration, deleted with the view).
        PreviewScale            scale;
        ChatScale*              scaler = nullptr;
        int                     number = 0; // the log's chat number
        // The cheap inputs of the scale as last seen (Paint compares these; QFontInfo runs only when one changed).
        qreal                   keyPoint  = -2;
        int                     keyPixel  = -2;
        QString                 keyFamily;
        int                     keyDpi    = 0;
        qreal                   keyDpr    = 0;
        int                     keyWidth  = -1;
        int                     keyPreset = 0;
        int                     keyIndent = -1;
        int                     keyBubble = -1;

        // Our preview objects in document order; rebuilt lazily after the document changed.
        bool                         positionsValid = false;
        QVector<PreviewPos>          previews;
        QHash<QString, QVector<int>> positionsByKey;
        QHash<QString, QSize>        formatSizes; // 2.5 zoom: du content size each preview was laid out for (gaps not included)
        QSet<QString>                resourced;   // keys with an image resource in this document
        QSet<QString>                stale;       // frames that changed while this chat was hidden
        QSet<QString>                overColumn;  // 2.5 zoom: wider than the column after a scale pass (laid out again)
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
        // 2.5 zoom: the content's size in du (rect is logical px: du x s) and the chat's scale. Hit tests of the art
        // modules get du: duSize and duAt().
        QSize        duSize;
        PreviewScale scale;
        QPointF      duAt(const QPointF& viewportPos) const { return scale.toDu(viewportPos - rect.topLeft()); }
    };

    void discover();
    void attachBrowser(QTextBrowser* browser);
    void attachInput(QWidget* input);
    void watchDocument(QTextBrowser* browser, QTextDocument* document);
    void scheduleScan(QTextBrowser* browser);
    void scan(QTextBrowser* browser);
    // keepPlace: a chat at its end stays at its end (chatscroll::toBottom). False inside a scan, which keeps the
    // reader's place once after all of its edits (2.6, R-DEF-1).
    void insertPreview(QTextBrowser* browser, int position, const QString& key, bool keepPlace = true);
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
    // 2.5 zoom: size is the content's du size (View::formatSizes, contentSizeOf); the rect is logical px.
    QRectF       previewRect(QTextBrowser* browser, int position, const QSizeF& size) const;
    bool         onScreen(QTextBrowser* browser, View& view, const QString& key);
    PreviewStyle styleFor(QTextBrowser* browser) const;
    static QString surfacesKey(const PreviewStyle& style); // 2.3: the bubbles under the previews, as a key
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
    // ---- 2.5 zoom ------------------------------------------------------------------------------------------
    // A chat's scale inputs changed maybe (Paint, the 300 ms pass, FontChange): compared cheaply, and a new scale
    // goes to the chat's ChatScale. force: compute it (a FontChange, the setting).
    void         checkScale(QTextBrowser* browser, bool force);
    // ChatScale's pass, in its one edit block: every preview object of ours gets its size for the new scale (no
    // drawing; the pictures are drawn again on screen). Then: render what is near the view.
    void         applyScale(QTextBrowser* browser, const PreviewScale& scale);
    void         afterScale(QTextBrowser* browser);
    void         logScale(QTextBrowser* browser, const PreviewScale& scale) const;
    void         attachScale(QTextBrowser* browser);
    // The budget swapped a picture of module for its stand-in: drawn again when it comes near the view.
    void         pictureSwapped(QTextBrowser* browser, const QString& name, int module);
    // The scale's sizes for an object of ours: the content's du size plus the gaps, as logical px.
    QSizeF       objectLogicalSize(const PreviewScale& scale, const QSize& duContent) const;
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
    // 2.2.1: what was typed into a chat input: nothing while it holds TeamSpeak's "Enter Chat Message..."
    // placeholder (chatinput.h), which must never become a caption or a reply.
    QString       typedText(QTextEdit* input) const;
    void          showDropOverlay(QWidget* widget, const QMimeData* mime);
    void          hideDropOverlay();
    void          showFeedback(QWidget* widget, const QString& text, bool error) const; // brief tooltip at the pointer
    // 2.5 GIFs: Save to GIFs / Remove from GIFs in a media menu (design_gifs 7).
    void          addGifMenu(QMenu* menu, const QString& key, const QUrl& webUrl, const linkpreview::Target& webTarget,
                             const std::function<QWidget*()>& viewport);
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
    void        refreshUnresourcedAlbums(QTextBrowser* browser); // grids without a picture yet drawn (keys taken first)
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

    // ---- 2.4 link previews ---------------------------------------------------------------------------------
    // key's picture for a preview object of a link's slot (its resource added, as insertPreview does) and the
    // object's size (the gaps included); empty when key has no entry.
    QSizeF linkPreviewObject(QTextBrowser* browser, const QString& key);

    // ---- chat redesign (chatlayout.cpp) -------------------------------------------------------------------
    // The first preview of block that can get reactions (an album: its grid), or empty.
    QString         reactableKeyIn(QTextBrowser* browser, int block);
    // The viewport rects of the pictures (previews, albums, cards) in block.
    QVector<QRectF> mediaRectsIn(QTextBrowser* browser, int block) const;

    Core*                      m_core;
    InlineMediaController*     m_media         = nullptr;
    VoiceController*           m_voice         = nullptr; // 2.2 voice
    QTimer*                    m_discoverTimer = nullptr;
    QTimer*                    m_visibilityTimer = nullptr;
    QHash<QTextBrowser*, View> m_views;
    bool                       m_scaling = false; // 2.5 zoom: inside a ChatScale pass (no own place restores, no drawing)
    int                        m_chatNumbers = 0;
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
    QSize                  m_pressDuSize; // 2.5 zoom: the pressed picture's du size and the chat's scale then
    PreviewScale           m_pressScale;
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
    ChatTextReactions*                      m_textReactions = nullptr; // 2.3 text reactions
    ChatReplies*                            m_replies   = nullptr; // 2.2 reply
    ChatEmoji*                              m_emoji     = nullptr; // 2.2 emoji: HD emoji in the chats, the input's emoji button
    ChatLayout*                             m_layout    = nullptr; // chat redesign: the Cozy / Compact layout
    ChatPolls*                              m_polls     = nullptr; // 2.3 poll
    ChatLinkPreviews*                       m_links     = nullptr; // 2.4 link previews
    // 2.3.4: the own-link handler and the press it took (TeamSpeak saw neither press nor release).
    std::function<bool(const QString&, bool)> m_ownLinkHandler;
    QPointer<GifLibrary>                      m_gifs; // 2.5 GIFs
    QString                                   m_ownLinkPressed;
    QPointer<QTextBrowser>                    m_ownLinkBrowser;
    bool                                      m_ownLinkDouble = false; // the press was a double click's

    bool filterOwnLink(QObject* watched, QEvent* event);
};
