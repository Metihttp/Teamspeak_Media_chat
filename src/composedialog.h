#pragma once

// 2.2 compose: the send window. Paste (Ctrl+V), drop and the file picker all open it: it shows what will
// be sent and where, takes a caption, marks pictures and videos as spoilers, offers "Send as an album"
// (when albums are enabled, see composehooks.h), says before Send what can't be sent and why, and hands
// everything to Core::send as one SendRequest.
//
// Window-modal over TeamSpeak's main window, deleted on close, objectName "tsmediaCompose" (plugin
// shutdown deletes it; its destructor waits for its thumbnail workers). Extension points for later
// versions: the per-item editor (Edit…) and the video Quality combo go into the item rows
// (ComposeDialog::Row) and SendItem; the presence line comes from compose::createPresenceLine().

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

class QCheckBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QMimeData;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

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

  private:
    class Row;
    class Thumb;
    class GlyphButton;
    class DropHint;

    void     startProbe(const compose::Item& item);
    // 2.4 compress: the planner's Quality entries for a probed video, and whether it can be sent.
    void     setVideoFacts(int id, const videocompress::VideoFacts& facts);
    void     planQuality(compose::Item& item);
    void     setQuality(int id, int index);
    bool     fitsCompressed(const compose::Item& item) const; // over the limit, but a picked quality fits
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
    QWidget* buildList();
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
    QPixmap  iconFor(const compose::Item& item, int size) const;
    void     announce(QWidget* widget); // screen readers: the text of widget changed

    ComposeHost                       m_host;
    ChatTarget                        m_target;
    QVector<compose::Item>            m_items;
    QHash<int, QImage>                m_thumbs; // id -> picture from the worker (device pixels, up to the large preview's size)
    int                               m_nextId      = 0;
    int                               m_limitMB     = 0;
    int                               m_previewMaxHeight = 280; // the large preview (less on small screens)
    int                               m_visibleRows = 5;        // list rows shown before it scrolls
    bool                              m_changed     = false; // items added or removed, spoiler or album changed
    bool                              m_overflow    = false; // more than compose::kMaxItems were offered
    bool                              m_dark        = false;
    bool                              m_sending     = false;
    bool                              m_targetChecked = false;
    bool                              m_keyboardFocus = false; // focus moved by keyboard: focus rings in dark skins
    bool                              m_applyingTheme = false;
    bool                              m_shown         = false;
    QString                           m_prefill;
    QString                           m_blocker; // shown in the banner; Send is off while set
    QString                           m_sendError;
    compose::LinkContext              m_linkContext;
    std::shared_ptr<std::atomic_bool> m_closing;
    int                               m_pending = 0; // probes not answered yet
    QThreadPool                       m_pool;

    // widgets
    QLabel*              m_header     = nullptr;
    QLabel*              m_serverNote = nullptr;
    QWidget*             m_presence   = nullptr;
    QWidget*             m_banner     = nullptr;
    QLabel*              m_bannerIcon = nullptr;
    QLabel*              m_bannerText = nullptr;
    QWidget*             m_itemsHost  = nullptr;
    QVBoxLayout*         m_itemsLayout = nullptr;
    QWidget*             m_itemsView  = nullptr; // what m_itemsHost shows now (single, list or empty)
    QScrollArea*         m_scroll     = nullptr;
    QHash<int, Row*>     m_rows;
    Thumb*               m_singleThumb = nullptr;
    QCheckBox*           m_singleSpoiler = nullptr;
    QWidget*             m_singleQuality = nullptr; // 2.4 compress: the single view's Quality combo
    int                  m_singleId    = -1;
    QLabel*              m_spoilerHint = nullptr;
    QPushButton*         m_addFiles   = nullptr;
    QCheckBox*           m_album      = nullptr;
    QLabel*              m_albumHint  = nullptr;
    QLineEdit*           m_caption    = nullptr;
    QLabel*              m_captionHelp = nullptr;
    QLabel*              m_counter    = nullptr;
    QLabel*              m_splitHint  = nullptr;
    QWidget*             m_warning    = nullptr;
    QLabel*              m_warningIcon = nullptr;
    QLabel*              m_warningText = nullptr;
    QPushButton*         m_send       = nullptr;
    QPushButton*         m_cancel     = nullptr;
    QTimer*              m_targetTimer = nullptr;
    DropHint*            m_dropHint   = nullptr;
    QPointer<QMessageBox> m_discard;
};
