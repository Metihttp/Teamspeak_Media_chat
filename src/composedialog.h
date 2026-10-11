#pragma once

// 2.2 compose: the send window. Paste (Ctrl+V), drop and the file picker all open it: it shows what will
// be sent and where, takes a caption, marks pictures and videos as spoilers, offers "Send as an album"
// (when albums are enabled, see composehooks.h), says before Send what can't be sent and why, and hands
// everything to Core::send as one SendRequest.
//
// 2.3 compose-v2: redesigned. A header with the destination, the presence line as a status row, one
// item as a large rounded preview (or a file card), several as a grid of tiles (thumbnail, type badge,
// name, size; spoiler / edit / remove on hover or keyboard focus), Edit and the video Quality as chips,
// "Send as an album" as a switch, the caption field with its emoji button, and a footer band with
// Add files, Send and Cancel. The controls are the plugin's own (composewidgets.h), coloured from
// TeamSpeak's palette; the title bar follows the skin (Windows 10/11).
//
// Window-modal over TeamSpeak's main window, deleted on close, objectName "tsmediaCompose" (plugin
// shutdown deletes it; its destructor waits for its thumbnail workers). Extension points for later
// versions: the per-item editor (Edit) and the video Quality chip go into the item tiles
// (ComposeDialog::Tile) and SendItem; the presence line comes from compose::createPresenceLine().

#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "composemodel.h"
#include "core.h"
#include "imageeditmodel.h" // 2.2 editor

class ImageEditor; // 2.2 editor
class QAbstractButton;
class QLabel;
class QLineEdit;
class QMessageBox;
class QMimeData;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace composeui { // 2.3 compose-v2
struct Theme;
class Button;
class Chip;
class SwitchRow;
} // namespace composeui

// What the send window needs from around it: ChatIntegration fills it in from TeamSpeak, Settings and
// Core; a render harness can use fakes. Every function is called on the GUI thread.
struct ComposeHost {
    std::function<QString(const ChatTarget&)>              describeTarget;  // "the channel “Lobby”"
    std::function<QString(const ChatTarget&)>              blocker;         // why nothing can be sent there now; empty: it can
    std::function<int(const ChatTarget&)>                  uploadLimitMB;   // the upload size limit that applies there
    std::function<compose::LinkContext(const ChatTarget&)> linkContext;     // for the caption size estimate
    std::function<QString(const QImage&)>                  savePastedImage; // a file for a pasted picture; empty if it failed
    std::function<int(const SendRequest&)>                 send;            // Core::send: the batch id, 0 if nothing was started
    std::function<void(bool)>                              rememberAlbum;   // the "Send as an album" choice, after a send
    bool                                                   albumDefault = true;
    // 2.4 compress: the compression settings (Core::compressOptions); unset: no Quality combo, videos over
    // the limit can't be sent.
    std::function<videocompress::Options()>                compressOptions;

    // 2.2 editor: pictures can be edited (crop & annotate) while editDirectory is set. Each edited copy
    // is written into a folder of its own under it; the window deletes them when it closes, and hands
    // Core separate links (or copies) when it sends. convertLargePngToJpeg: the paste rule for the
    // edited file. The editor starts with editorPrefs and reports its last choices to rememberEditorPrefs.
    QString                                                editDirectory;
    bool                                                   convertLargePngToJpeg = true;
    imageedit::Prefs                                       editorPrefs;
    std::function<void(const imageedit::Prefs&)>           rememberEditorPrefs;

    // 2.2 reply: "Replying to Alice" under the header while the files would go out as a reply (send()
    // adds the quote line); nullptr: not a reply. The widget deletes itself when the user drops the reply.
    std::function<QWidget*(QWidget*, const ChatTarget&)> replyLine;

    // 2.3 compose-v2: the file picker of "Add files…" (and Ctrl+O): the files chosen, empty when none.
    // Unset: Windows' file dialog (QFileDialog). Tests and render tools answer it themselves.
    std::function<QStringList(QWidget* parent, const QString& title, const QString& folder, const QString& filter)> pickFiles;
};

class ComposeDialog : public QDialog
{
    Q_OBJECT

  public:
    ComposeDialog(ComposeHost host, const ChatTarget& target, QWidget* parent);
    ~ComposeDialog() override; // waits for its thumbnail workers

    const ChatTarget& target() const { return m_target; }

    // Items are added in this order; at most compose::kMaxItems. Files the window already has are skipped.
    void addFiles(const QStringList& paths);
    void addImage(const QImage& image);
    // Text typed in the chat input before Ctrl+V: the caption to start with (not a change of the user's:
    // closing the window doesn't ask about it).
    void setPrefilledCaption(const QString& caption);
    // "Add files…": the file picker, then the chosen files are added.
    void addFromPicker();

    // For render tools and tests.
    int        itemCount() const { return m_items.size(); }
    void       setSpoiler(int index, bool spoiler);
    void       setAlbum(bool album);
    bool       isBusy() const; // thumbnails are still being made
    void       setQualityIndex(int index, int quality); // 2.4 compress: picks a Quality entry (render tools)
    QVector<videocompress::Choice> qualityChoices(int index) const;
    QLineEdit* captionField() const { return m_caption; }
    void       showDropTarget(bool shown);

    // 2.2 editor: Edit… (the pencil, E on a focused row) opens the editor for a picture; its result
    // replaces the item ("Edited"), and "Revert to original" brings the original back.
    void         editItem(int index);
    ImageEditor* editor() const;
    QString      editBlocker(int id) const; // why the item can't be edited; empty: it can
    bool         isEdited(int index) const;
    const compose::Item& itemAt(int index) const { return m_items.at(index); } // render tools and tests

  signals:
    // The request reached Core. caption: what the caption field held (empty: no caption); batch: what
    // Core::send returned (Core::captionSettled reports the caption under it).
    void sent(const QString& caption, int batch);

  public slots:
    void reject() override; // Esc, Cancel and the close button: asks first when the user changed something

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override; // 2.3 compose-v2: the background and the footer band

  private:
    class Tile;      // 2.3 compose-v2: one item (grid tile, large preview or file card)
    class DropHint;
    class EmptyZone; // 2.3 compose-v2: nothing added yet
    class Callout;   // 2.3 compose-v2: why nothing can be sent / what is skipped
    class Badge;     // 2.3 compose-v2: the destination's mark in the header

    void     startProbe(const compose::Item& item);
    // 2.4 compress: the planner's Quality entries for a probed video, and whether it can be sent.
    void     setVideoFacts(int id, const videocompress::VideoFacts& facts);
    void     planQuality(compose::Item& item);
    void     setQuality(int id, int index);
    bool     fitsCompressed(const compose::Item& item) const; // over the limit, but a picked quality fits
    void     openQualityMenu(int id, QWidget* anchor);        // 2.3 compose-v2: the Quality chip's menu
    void     stepQuality(int id, int delta);                  // 2.3 compose-v2: Up / Down on the chip
    void     chooseQuality(int id, int index);                // 2.3 compose-v2: from the menu or the keys
    // A worker looked at an item: readable, its size in pixels, its length, a picture of it.
    void     onProbed(int id, bool readable, const QSize& pixels, qint64 durationMs, const QImage& thumb);
    void     removeItem(int id);
    void     toggleSpoiler(int id);
    int      indexOf(int id) const;
    bool     acceptsMime(const QMimeData* mime) const;
    bool     addMime(const QMimeData* mime); // false: nothing in it to add
    bool     clipboardHasMedia() const;
    void     pasteFromClipboard();
    void     rebuildItems(int focusIndex = -1);
    QWidget* buildSingle(const compose::Item& item);
    QWidget* buildGrid();
    void     refreshThumb(int id);
    bool     restat();          // files changed on disk meanwhile (size, gone); true: something changed
    void     refreshTarget();   // connected? partner still there?
    void     updateState();     // counts, labels, hints, Send
    void     updateCaption();   // counter, split hint
    void     fitHeight();
    void     send();
    bool     isDirty() const;
    void     applyTheme();
    QImage   thumbFor(int id, const QSize& logical, bool cover) const;
    void     announce(QWidget* widget); // screen readers: the text of widget changed
    int      contentWidth() const;      // 2.3 compose-v2
    int      gridColumns() const;
    int      tileSide(bool scrolls) const;
    int      gridHeight(int rows) const;
    // 2.3 compose-v2 review: the item control that has the keyboard focus (its item and objectName), so a
    // rebuilt view gives it back instead of the focus falling through to the caption.
    struct FocusSpot {
        int     id     = -1;
        QString name;
        bool    inView = false;
    };
    FocusSpot focusSpot() const;
    bool      restoreFocus(const FocusSpot& spot);

    // 2.2 editor
    struct EditRecord {
        compose::Item             original;      // the item before its first edit (Revert)
        QImage                    originalThumb;
        imageedit::ImageEditModel model;         // the edits, without the picture's pixels
        QString                   folder;        // the edited file's own folder (files only)
    };
    bool    editingOffered() const { return !m_host.editDirectory.isEmpty(); }
    void    openEditor(int id);
    void    onEditOpened(int id, int generation, const QImage& picture, int problem);
    void    saveEdit();
    void    onEditSaved(int id, int generation, const imageedit::ImageEditModel& model, const QString& folder, const QString& path,
                        const QImage& picture, const QImage& thumb, const QString& error);
    void    askRevert(int id);
    void    revertEdit(int id);
    void    focusEdit(int id);
    QString handOver(const compose::Item& item, QStringList* folders) const; // a link (or copy) of the edited file for Core

    ComposeHost                       m_host;
    ChatTarget                        m_target;
    QVector<compose::Item>            m_items;
    QHash<int, QImage>                m_thumbs; // id -> picture from the worker (device pixels, up to the large preview's size)
    int                               m_nextId      = 0;
    int                               m_limitMB     = 0;
    int                               m_previewMaxHeight = 320; // the large preview (less on small screens)
    int                               m_visibleRows = 2;        // grid rows shown before it scrolls
    int                               m_gridRows    = 0;        // 2.3 compose-v2: rows of the grid shown now
    QVector<int>                      m_rowHeights;             // its rows' heights (a video's row has its Quality chip)
    bool                              m_gridScrolls = false;    // its tiles leave room for the scroll bar
    bool                              m_gridSqueezed = false;   // a small screen shows only part of one row
    bool                              m_refitting   = false;
    bool                              m_fitQueued   = false; // 2.3: the presence line changed height: fitHeight() soon
    bool                              m_changed     = false; // items added or removed, spoiler or album changed
    bool                              m_overflow    = false; // more than compose::kMaxItems were offered
    bool                              m_dark        = false;
    bool                              m_sending     = false;
    bool                              m_targetChecked = false;
    bool                              m_applyingTheme = false;
    bool                              m_shown         = false;
    bool                              m_picking       = false; // 2.3 compose-v2: the file picker is open
    QString                           m_prefill;
    QString                           m_blocker; // shown in the banner; Send is off while set
    QString                           m_sendError;
    compose::LinkContext              m_linkContext;
    std::shared_ptr<std::atomic_bool> m_closing;
    int                               m_pending = 0; // probes not answered yet
    QThreadPool                       m_pool;

    // widgets
    Badge*               m_badge      = nullptr;
    QLabel*              m_header     = nullptr;
    QLabel*              m_serverNote = nullptr;
    QLabel*              m_summary    = nullptr; // 2.3 compose-v2: "4 images · 12.3 MB" under the header
    QWidget*             m_presence   = nullptr;
    Callout*             m_banner     = nullptr;
    QWidget*             m_itemsHost  = nullptr;
    QVBoxLayout*         m_itemsLayout = nullptr;
    QWidget*             m_itemsView  = nullptr; // what m_itemsHost shows now (single, grid or empty)
    QScrollArea*         m_scroll     = nullptr;
    QHash<int, Tile*>    m_tiles;
    Tile*                m_singleTile  = nullptr;
    QAbstractButton*     m_singleSpoiler = nullptr;
    composeui::Chip*     m_singleQuality = nullptr; // 2.4 compress: the single view's Quality chip
    int                  m_singleId    = -1;
    composeui::Button*   m_addFiles   = nullptr;
    composeui::SwitchRow* m_album     = nullptr;
    QLineEdit*           m_caption    = nullptr;
    QWidget*             m_captionNotes = nullptr;
    QLabel*              m_captionHelp = nullptr;
    QLabel*              m_counter    = nullptr;
    QLabel*              m_splitHint  = nullptr;
    Callout*             m_warning    = nullptr;
    QWidget*             m_footer     = nullptr;
    composeui::Button*   m_send       = nullptr;
    composeui::Button*   m_cancel     = nullptr;
    QTimer*              m_targetTimer = nullptr;
    DropHint*            m_dropHint   = nullptr;
    QPointer<QMessageBox> m_discard;

    // 2.2 editor
    QHash<int, EditRecord> m_edits;
    QPointer<ImageEditor>  m_editor;
    QPointer<QMessageBox>  m_revertBox;
    composeui::Chip*       m_singleEdit     = nullptr;
    composeui::Chip*       m_singleRevert   = nullptr;
    int                    m_editingId      = -1;
    int                    m_editGeneration = 0;
    int                    m_editJobs       = 0; // opening or saving on the pool
};
