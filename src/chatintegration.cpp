#include "chatintegration.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImageIOHandler>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "audiocard.h" // 2.2 audio
#include "filedrag.h" // 2.2 drag-out
#include "albums.h" // 2.2 album
#include "chatreactions.h" // 2.2 reactions
#include "i18n.h"
#include "inlinemedia.h"
#include "mediaviewer.h"
#include "ownedtimer.h"
#include "settings.h"
#include "spoiler.h" // 2.2 spoiler
#include "uiutil.h"
#include "uploadtoast.h"
// 2.2 compose
#include <QTextEdit>

#include "composedialog.h"
#include "composehooks.h"
#include "composemodel.h"
#include "voicecard.h"       // 2.2 voice
#include "voicecontroller.h" // 2.2 voice

namespace {

const QString kScheme = QStringLiteral("tsmedia:");

// Breathing room between the message line and its preview (and below it), in logical pixels. The
// gap is transparent padding inside the image object: a preview's QTextImageFormat is the content
// size plus both gaps. Geometry helpers work on the content size (contentSizeOf).
constexpr int kGapTop    = 14;
constexpr int kGapBottom = 4;

// Marks the line breaks insertPreview puts around a preview, so they can be told apart from the
// chat's own line breaks when previews are removed again.
constexpr int kSeparatorProperty = QTextFormat::UserProperty + 0x7453;

// Content (drawn picture) size of a preview object, without the transparent gaps.
QSizeF contentSizeOf(const QTextImageFormat& format)
{
    return QSizeF(format.width(), qMax(0.0, format.height() - kGapTop - kGapBottom));
}

QTextCharFormat separatorFormat()
{
    QTextCharFormat format;
    format.setProperty(kSeparatorProperty, true);
    return format;
}

// Format of the character at position.
QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

bool isOurSeparator(QTextDocument* doc, int position)
{
    return position >= 0 && doc->characterAt(position) == QChar::LineSeparator && formatAt(doc, position).boolProperty(kSeparatorProperty);
}

QPixmap paddedPixmap(const QImage& content)
{
    const qreal dpr = content.devicePixelRatio();
    QImage      out(content.width(), content.height() + qRound((kGapTop + kGapBottom) * dpr), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(QPointF(0, kGapTop), content);
    p.end();
    return QPixmap::fromImage(out);
}

constexpr qint64 kMaxCopyPixels = 80LL * 1000 * 1000;

bool hasChatAncestor(const QWidget* w)
{
    for (const QWidget* p = w->parentWidget(); p; p = p->parentWidget()) {
        if (p->inherits("ChatTab") || p->inherits("ChatTabWidget") || p->inherits("MainWindowChatWidget"))
            return true;
    }
    return false;
}

struct TabText {
    QString text;
    bool    elided = false; // the client shortened it ("..."): text is only a prefix of the name
};

// Tab texts use '&' as the mnemonic marker ("&&" is a literal '&').
TabText cleanTabText(const QString& raw)
{
    TabText tab;
    tab.text.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) != QLatin1Char('&')) {
            tab.text += raw.at(i);
        } else if (i + 1 < raw.size() && raw.at(i + 1) == QLatin1Char('&')) {
            tab.text += QLatin1Char('&');
            ++i;
        }
    }
    tab.text = tab.text.trimmed();
    if (tab.text.endsWith(QStringLiteral("..."))) {
        tab.text.chop(3);
        tab.elided = true;
    } else if (tab.text.endsWith(QChar(0x2026))) {
        tab.text.chop(1);
        tab.elided = true;
    }
    tab.text = tab.text.trimmed();
    return tab;
}

bool tabShows(const TabText& tab, const QString& name)
{
    if (tab.text.isEmpty() || name.isEmpty())
        return false;
    return tab.text == name || (tab.elided && name.startsWith(tab.text));
}

QString nicknameOf(uint64 sch, anyID client)
{
    char* name = nullptr;
    if (!ts3::funcs.getClientVariableAsString || ts3::funcs.getClientVariableAsString(sch, client, CLIENT_NICKNAME, &name) != ERROR_ok)
        return {};
    return ts3::takeString(name);
}

// The client a private chat tab is with: the exact nickname, or, for a shortened tab text, the only
// nickname it is the beginning of. 0 when that is not known (the partner left, was renamed, ...).
anyID clientForTab(uint64 sch, const TabText& tab)
{
    if (tab.text.isEmpty())
        return 0;
    if (const anyID exact = ts3::clientIdByNickname(sch, tab.text))
        return exact;
    if (!tab.elided || !ts3::funcs.getClientList || !ts3::funcs.freeMemory)
        return 0;
    anyID* clients = nullptr;
    if (ts3::funcs.getClientList(sch, &clients) != ERROR_ok || !clients)
        return 0;
    const anyID own     = ts3::ownClientId(sch);
    anyID       found   = 0;
    int         matches = 0;
    for (anyID* it = clients; *it; ++it) {
        if (*it != own && nicknameOf(sch, *it).startsWith(tab.text)) {
            found = *it;
            ++matches;
        }
    }
    ts3::funcs.freeMemory(clients);
    return matches == 1 ? found : 0;
}

bool isAtBottom(QTextBrowser* b)
{
    const QScrollBar* sb = b->verticalScrollBar();
    return sb->value() >= sb->maximum() - 4;
}

bool isMediaKind(MediaKind kind)
{
    return kind == MediaKind::Video || isPreviewableImage(kind);
}

// Key of a TS Media preview object, empty for anything else.
QString previewKeyOf(const QTextCharFormat& format)
{
    if (!format.isImageFormat())
        return {};
    const QString name = format.toImageFormat().name();
    return name.startsWith(kScheme) ? name.mid(kScheme.length()) : QString();
}

bool isTs3FileAnchor(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

// Pixels Core decodes the still at: the preview box (TS Media links) or the maximum box.
QSize stillPixelsFor(const MediaEntry& e, const PreviewStyle& style)
{
    const bool hasDims = e.link.width > 0 && e.link.height > 0;
    return (hasDims || e.kind == MediaKind::Video) ? previewLogicalSize(e, style) * style.dpr : QSize(style.maxWidth, style.maxHeight) * style.dpr;
}

// The video has started (a frame, a position): renderVideo() draws its control bar then.
bool videoStarted(const PlaybackOverlay& o, bool hasFrame)
{
    return hasFrame || o.playing || o.ended || o.positionMs > 0;
}

} // namespace

ChatIntegration::ChatIntegration(Core* core, QObject* parent)
    : QObject(parent)
    , m_core(core)
{
}

ChatIntegration::~ChatIntegration()
{
    // 2.2 voice: first, while the players and Core exist: a recording is canceled, the mic given back.
    delete m_voice;
    m_voice = nullptr;
    // 2.2 reactions: its picker and hover state go first.
    delete m_reactions;
    m_reactions = nullptr;
    // 2.2 album: the chats stay with TeamSpeak: messages we hid are shown and collapsed album links
    // given back (the grids stay, like single previews). m_mutating stays set: no rescan is queued now.
    m_mutating = true;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser && it->document == it->browser->document())
            restoreAlbums(it->browser);
    }
    // Inline players are shut down here, synchronously, while Core still exists.
    delete m_media;
    m_media = nullptr;
    if (m_compose) // 2.2 compose: waits for its thumbnail workers (code of this DLL)
        delete m_compose.data();
    if (m_dropOverlay)
        delete m_dropOverlay.data();

    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser)
            it->browser->viewport()->removeEventFilter(this);
    }
    for (const auto& input : qAsConst(m_inputs)) {
        if (!input)
            continue;
        input->removeEventFilter(this);
        if (auto* area = qobject_cast<QAbstractScrollArea*>(input.data()))
            area->viewport()->removeEventFilter(this);
    }
    if (m_toast)
        delete m_toast.data();
    if (m_cursorOwner)
        m_cursorOwner->setCursor(m_savedCursor);
}

void ChatIntegration::start()
{
    // Queued: Core may emit while we are iterating a document.
    connect(m_core, &Core::entryChanged, this, &ChatIntegration::onEntryChanged, Qt::QueuedConnection);
    connect(m_core, &Core::uploadChanged, this, &ChatIntegration::onUploadChanged, Qt::QueuedConnection);
    connect(m_core, &Core::openRequested, this, &ChatIntegration::onOpenRequested, Qt::QueuedConnection);
    connect(m_core, &Core::captionSettled, this, &ChatIntegration::onCaptionSettled, Qt::QueuedConnection); // 2.2 compose

    // Created after our own entryChanged connection, so previews are re-laid out (and frame sizes
    // updated) before the controller reacts to the same change.
    m_media = new InlineMediaController(m_core, this);
    connect(m_media, &InlineMediaController::frameChanged, this, &ChatIntegration::onFrameChanged);
    connect(m_media, &InlineMediaController::openRequested, this, &ChatIntegration::openViewer, Qt::QueuedConnection);
    connect(m_media, &InlineMediaController::playbackStarted, this, [this] {
        if (m_viewer)
            m_viewer->pausePlayback();
        emit playbackStarted(); // 2.2 voice
    });
    // 2.2 spoiler: each step of a reveal crossfade is redrawn like a frame.
    m_reveals = new RevealFades(this);
    connect(m_reveals, &RevealFades::changed, this, &ChatIntegration::onFrameChanged);
    m_reactions = new ChatReactions(this, m_core); // 2.2 reactions
    m_voice = new VoiceController(this, m_core, this); // 2.2 voice

#ifdef TSMEDIA_TESTHOOKS
    QFile options(ts3::dataDir() + QStringLiteral("/selftest_options.txt"));
    if (options.open(QIODevice::ReadOnly)) {
        m_testRawChat = QString::fromUtf8(options.readAll()).contains(QLatin1String("nohide"), Qt::CaseInsensitive);
        if (m_testRawChat)
            ts3::log(QStringLiteral("[test] nohide: plugin-required notes stay visible and no previews are inserted for localhost links"));
    }
#endif

    m_discoverTimer = new QTimer(this);
    m_discoverTimer->setInterval(1500);
    connect(m_discoverTimer, &QTimer::timeout, this, &ChatIntegration::discover);
    m_discoverTimer->start();

    m_visibilityTimer = new QTimer(this);
    m_visibilityTimer->setInterval(300);
    connect(m_visibilityTimer, &QTimer::timeout, this, &ChatIntegration::updateVisibleKeys);
    m_visibilityTimer->start();

    discover();
}

QWidget* ChatIntegration::mainWindow() const
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    }
    return QApplication::activeWindow();
}

// ============================================================================================
// Discovery
// ============================================================================================

void ChatIntegration::discover()
{
    m_inputs.removeAll(QPointer<QWidget>());

    const auto widgets = QApplication::allWidgets();
    for (QWidget* w : widgets) {
        if (auto* browser = qobject_cast<QTextBrowser*>(w)) {
            if (!m_views.contains(browser) && hasChatAncestor(browser))
                attachBrowser(browser);
        } else if (w->inherits("ChatLineEdit")) {
            bool known = false;
            for (const auto& input : qAsConst(m_inputs))
                known = known || input == w;
            if (!known)
                attachInput(w);
        }
    }

    // The client may swap documents (e.g. when reloading history); rescan when that happens.
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        View& v = it.value();
        if (v.browser && v.document != v.browser->document()) {
            v.document       = v.browser->document();
            v.positionsValid = false;
            v.resourced.clear();
            v.stale.clear();
            watchDocument(v.browser, v.document);
            scheduleScan(v.browser);
        }
    }

    // Lines Core prints (upload results, warnings) use prefix colours for the chat's theme.
    if (QTextBrowser* chat = visibleChatBrowser())
        ts3::setChatDark(styleFor(chat).dark);
}

void ChatIntegration::attachBrowser(QTextBrowser* browser)
{
    View v;
    v.browser  = browser;
    v.document = browser->document();
    m_views.insert(browser, v);

    connect(browser, &QObject::destroyed, this, [this, browser] {
        m_views.remove(browser);
        scheduleVisibilityUpdate(); // players whose preview went with it are stopped
    });
    watchDocument(browser, v.document);
    connect(browser->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, b = QPointer<QTextBrowser>(browser)] {
        scheduleVisibilityUpdate();
        // The chat moved under a still pointer (wheel, a new message): Qt sends no mouse move for that.
        // Deferred: refreshPreview / insertPreview scroll the chat themselves.
        QTimer::singleShot(0, this, [this, b] {
            if (b && m_media && !m_seeking && b->viewport()->underMouse())
                updateHoverAt(b, QPointF(b->viewport()->mapFromGlobal(QCursor::pos())), false);
        });
    });

    browser->viewport()->installEventFilter(this);
    browser->viewport()->setAcceptDrops(true);
    browser->viewport()->setMouseTracking(true);
    scheduleScan(browser);
}

void ChatIntegration::watchDocument(QTextBrowser* browser, QTextDocument* document)
{
    connect(document, &QTextDocument::contentsChange, this, [this, browser](int, int, int) {
        auto it = m_views.find(browser);
        if (it != m_views.end())
            it->positionsValid = false;
        if (!m_mutating)
            scheduleScan(browser);
    });
}

void ChatIntegration::attachInput(QWidget* input)
{
    m_inputs.append(input);
    input->installEventFilter(this);
    input->setAcceptDrops(true);
    if (auto* area = qobject_cast<QAbstractScrollArea*>(input)) {
        area->viewport()->installEventFilter(this);
        area->viewport()->setAcceptDrops(true);
    }
}

ChatIntegration::View* ChatIntegration::viewFor(QTextBrowser* browser)
{
    if (!browser)
        return nullptr;
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

QTextBrowser* ChatIntegration::browserForViewport(QObject* viewport) const
{
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && it->browser->viewport() == viewport)
            return it->browser;
    }
    return nullptr;
}

// ============================================================================================
// Scanning chat documents: hide notes, insert previews
// ============================================================================================

void ChatIntegration::scheduleScan(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    if (it == m_views.end() || it->scanQueued)
        return;
    it->scanQueued = true;
    QPointer<QTextBrowser> guard(browser);
    singleShotOwned(60, this, [this, guard] {
        if (!guard)
            return;
        auto vt = m_views.find(guard.data());
        if (vt == m_views.end())
            return;
        vt->scanQueued = false;
        scan(guard.data());
    });
}

bool ChatIntegration::testShowsRawChat(const MediaLink& link) const
{
#ifdef TSMEDIA_TESTHOOKS
    if (m_testRawChat) {
        const QString host = link.host.toLower();
        return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1") || host == QLatin1String("[::1]");
    }
#endif
    Q_UNUSED(link);
    return false;
}

void ChatIntegration::scan(QTextBrowser* browser)
{
    QTextDocument* doc        = browser->document();
    View*          view       = viewFor(browser);
    const bool     previewsOn = Settings::instance().inlinePreviews;
    if (!view)
        return;
#ifdef TSMEDIA_TESTHOOKS
    if (m_testRawChat)
        requestSnapshot(browser, QStringLiteral("raw chat"));
#endif

    struct Link {
        int       start = 0;
        int       end   = 0;
        MediaLink link;
        QString   key;
    };
    // Document edits, applied back to front so earlier positions stay valid.
    struct Op {
        int     position = 0;
        int     length   = 0; // > 0: remove that many characters; 0: insert the preview of key
        QString key;
    };
    QVector<Op>   ops;
    QSet<QString> present;
    int           hiddenNotes = 0;
    // 2.2 album: the items of an album get no preview of their own (applyAlbums draws their grid).
    const AlbumPlan albumPlan = planAlbums(browser);

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QVector<Link> links;
        QVector<int>  anchorStarts; // every ts3file anchor, to bound the note of the previous one
        QSet<QString> previews;
        QString       lastHref;
        int           lastEnd = -1;

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            const QTextCharFormat cf  = f.charFormat();
            const QString         key = previewKeyOf(cf);
            if (!key.isEmpty()) {
                previews.insert(key);
                continue;
            }
            if (!isTs3FileAnchor(cf))
                continue;

            const QString href  = cf.anchorHref();
            const int     start = f.position();
            const int     end   = start + f.length();
            if (href == lastHref && start == lastEnd) {
                if (!links.isEmpty() && links.last().end == start)
                    links.last().end = end; // same link split over several fragments
                lastEnd = end;
                continue;
            }
            anchorStarts.append(start);
            lastHref = href;
            lastEnd  = end;
            const MediaLink link = MediaLink::parse(href);
            if (link.isValid())
                links.append({start, end, link, link.key()});
        }
        if (links.isEmpty())
            continue;

        const int blockEnd = block.position() + block.length() - 1;
        for (const Link& hit : qAsConst(links)) {
            const bool raw        = testShowsRawChat(hit.link);
            const bool hasPreview = previews.contains(hit.key);
            if (hasPreview)
                present.insert(hit.key);

            if (previewsOn && !raw)
                m_core->ensure(hit.link);

            // The "plugin required" note after a TS Media link is meant for clients without
            // the plugin: delete it (everything up to the next ts3file link or the block end,
            // except our own preview objects and line breaks).
            if (hit.link.isTsMedia() && !raw) {
                int limit = blockEnd;
                for (int start : qAsConst(anchorStarts)) {
                    if (start >= hit.end) {
                        limit = start;
                        break;
                    }
                }
                bool removed = false;
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment f = it.fragment();
                    if (!f.isValid())
                        continue;
                    const int fStart = f.position();
                    const int fEnd   = fStart + f.length();
                    if (fEnd <= hit.end || fStart >= limit || !previewKeyOf(f.charFormat()).isEmpty())
                        continue;
                    const QString text     = f.text();
                    int           runStart = -1;
                    const int     from     = qMax(hit.end, fStart);
                    const int     to       = qMin(limit, fEnd);
                    for (int p = from; p < to; ++p) {
                        if (text.at(p - fStart) == QChar::LineSeparator) {
                            if (runStart >= 0)
                                ops.append({runStart, p - runStart, QString()});
                            runStart = -1;
                        } else if (runStart < 0) {
                            runStart = p;
                        }
                    }
                    if (runStart >= 0)
                        ops.append({runStart, to - runStart, QString()});
                    removed = removed || from < to;
                }
                // A line break we put after the preview would end the block once the note is
                // gone (an empty line): drop it too. Everything else in the range is removed, so
                // the last kept character is either that line break or the preview itself.
                if (removed && limit == blockEnd) {
                    for (int p = limit - 1; p > hit.end; --p) {
                        const QChar ch = doc->characterAt(p);
                        if (ch == QChar::ObjectReplacementCharacter)
                            break;
                        if (ch != QChar::LineSeparator)
                            continue;
                        // Untagged separators right after a preview come from older plugin versions.
                        if (isOurSeparator(doc, p) || !previewKeyOf(formatAt(doc, p - 1)).isEmpty())
                            ops.append({p, 1, QString()});
                        break;
                    }
                }
                if (removed)
                    ++hiddenNotes;
            }

            // 2.2 album: an album item, or another link to one in the same message (the grid shows it).
            if (!hasPreview && previewsOn && !raw && !albumPlan.members.contains(hit.start) && !albumPlan.memberKeys.value(block.blockNumber()).contains(hit.key))
                ops.append({hit.end, 0, hit.key});
        }
    }

    // Previews that survived a document rebuild (or a plugin reload) get their image resource back,
    // and their layout size is corrected if it no longer matches (e.g. other gaps or chat width).
    for (const QString& key : qAsConst(present)) {
        if (!view->resourced.contains(key))
            refreshPreview(browser, key, false);
    }

    if (ops.isEmpty()) {
        applyAlbums(browser, albumPlan); // 2.2 album
        if (!present.isEmpty())
            browser->viewport()->update();
        return;
    }

    // Back to front; at the same position the note removal goes before the preview insertion.
    std::sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        if (a.position != b.position)
            return a.position > b.position;
        return a.length > b.length;
    });

    const bool bottom = isAtBottom(browser);
    for (const Op& op : qAsConst(ops)) {
        if (op.length > 0) {
            m_mutating = true;
            QTextCursor c(doc);
            c.setPosition(op.position);
            c.setPosition(op.position + op.length, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            m_mutating = false;
        } else {
            insertPreview(browser, op.position, op.key);
        }
    }
    if (bottom)
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
    if (albumPlan.active)
        applyAlbums(browser, planAlbums(browser)); // 2.2 album: planned again, the positions moved

    if (hiddenNotes > 0) {
#ifdef TSMEDIA_TESTHOOKS
        ts3::log(QStringLiteral("[test] hid %1 plugin-required note(s)").arg(hiddenNotes));
#endif
        requestSnapshot(browser, QStringLiteral("notes hidden"));
    }
    scheduleVisibilityUpdate();
}

PreviewStyle ChatIntegration::styleFor(QTextBrowser* browser) const
{
    const Settings& s = Settings::instance();
    PreviewStyle    style;
    // TeamSpeak themes are style sheets; light text means a dark chat even if Base was not set.
    style.dark = browser->viewport()->palette().color(QPalette::Base).lightness() < 128
                 || browser->palette().color(QPalette::Text).lightness() > 170;
    style.font = browser->font();
    if (style.font.pointSizeF() <= 0 && style.font.pixelSize() <= 0)
        style.font.setPointSizeF(9);
    style.dpr       = browser->devicePixelRatioF();
    style.maxWidth  = qMin(s.previewMaxWidth, qMax(160, previewAreaWidth(browser) - 48));
    style.maxHeight = s.previewMaxHeight;
    style.animate   = ui::animationsEnabled();
    return style;
}

// The viewport width as if the vertical scroll bar were shown. Previews sized by the actual width
// could keep toggling the bar: wider previews -> taller chat -> bar appears -> narrower previews ->
// shorter chat -> bar disappears -> ... With this width the bar never changes the layout.
int ChatIntegration::previewAreaWidth(QTextBrowser* browser) const
{
    const int viewportWidth = browser->viewport()->width();
    if (browser->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff)
        return viewportWidth;
    const QScrollBar* bar = browser->verticalScrollBar();
    if (bar->isVisibleTo(browser)) {
        // What the bar really takes (style sheets, spacing), for when it is hidden again.
        m_scrollBarExtent = qMax(0, browser->maximumViewportSize().width() - viewportWidth);
        return viewportWidth;
    }
    return viewportWidth - (m_scrollBarExtent > 0 ? m_scrollBarExtent : bar->sizeHint().width());
}

QImage ChatIntegration::renderFor(QTextBrowser* browser, const QString& key, QSize* logicalSize)
{
    if (albums::isObjectId(key))
        return renderAlbumFor(browser, key, logicalSize); // 2.2 album
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return {};

    PreviewStyle style   = styleFor(browser);
    const bool   hasDims = e->link.width > 0 && e->link.height > 0;
    const bool   media   = isMediaKind(e->kind);
    // Pointer feedback: hovered while the mouse is over it, pressed while the left button that went
    // down on it is held there (a seek drag is not a press of the picture).
    // Only in the chat it is hovered in: the same file in another chat (or tab) looks normal.
    // 2.2 album: not while it is hovered in an album (the same file can be a tile of a grid too).
    style.hovered    = !m_hoverKey.isEmpty() && key == m_hoverKey && browser == m_hoverBrowser && m_hoverObject == key;
    style.pressed    = style.hovered && key == m_pressedKey && browser == m_pressBrowser && !m_seeking;
    style.revealOnly = !media && e->state == MediaState::Ready && m_core->isUnsafeToOpen(key);
    // 2.2 spoiler: covered while hidden, then the crossfade after a reveal.
    style.concealOpacity = spoilerCoverOpacity(key);
    if (media && style.concealOpacity <= 0.0 && browser->isVisible())
        m_core->noteShownOpen(key);
    const QSize stillPixels = stillPixelsFor(*e, style);
    QSize       size;
    QImage      img;
    bool        fromStill = false;

    const InlineMediaController::Mode mode = m_media ? m_media->mode(key) : InlineMediaController::Mode::Still;
    if (mode == InlineMediaController::Mode::Video) {
        const QImage     frame  = m_media->frame(key);
        const MediaStill poster = frame.isNull() ? m_core->still(key, stillPixels) : MediaStill();
        PlaybackOverlay  o      = m_media->overlay(key);
        if (browser != m_hoverBrowser)
            o.hover = VideoZone::None; // the pointer is over this video in another chat
        if (style.pressed) // hidden controls can't be aimed at: the press is on the picture
            o.pressed = m_pressControlsVisible ? o.hover : VideoZone::Body;
        img = renderVideo(*e, frame, poster, o, style, &size);
    } else if (mode == InlineMediaController::Mode::Audio) { // 2.2 audio: the player card
        PlaybackOverlay o = m_media->overlay(key);
        if (browser != m_hoverBrowser)
            o.hover = VideoZone::None; // the pointer is over this card in another chat
        o.pressed = style.pressed ? o.hover : VideoZone::None;
        img       = renderAudioCard(*e, o, style, &size);
        m_media->setFrameSize(key, size * style.dpr); // paces the card's progress repaints
    } else if (mode == InlineMediaController::Mode::Animated) {
        const QImage frame = m_media->frame(key);
        if (!frame.isNull())
            img = renderAnimatedFrame(*e, frame, style, &size);
    }
    if (img.isNull()) {
        img       = renderPreview(*e, media ? m_core->still(key, stillPixels) : MediaStill(), style, &size);
        fromStill = true;
    }

    // Frames are produced at the size they are shown at. Without link dimensions the still decides
    // the layout (frames must not change it).
    if (m_media && media) {
        if (hasDims || e->kind == MediaKind::Video)
            m_media->setFrameSize(key, previewLogicalSize(*e, style) * style.dpr);
        else if (fromStill)
            m_media->setFrameSize(key, size * style.dpr);
    }
    // 2.2 reactions: the reaction row under the media and the add button (outside previewLogicalSize).
    if (m_reactions)
        img = m_reactions->compose(browser, key, img, style, &size);
    if (logicalSize)
        *logicalSize = size;
    return img;
}

void ChatIntegration::insertPreview(QTextBrowser* browser, int position, const QString& key)
{
    QSize        size;
    const QImage img = renderFor(browser, key, &size);
    if (img.isNull())
        return;

    QTextDocument* doc    = browser->document();
    const bool     bottom = isAtBottom(browser);
    const QUrl     url(kScheme + key);
    doc->addResource(QTextDocument::ImageResource, url, paddedPixmap(img));
    if (View* view = viewFor(browser)) {
        view->resourced.insert(key);
        // While a relayout is pending, the older previews still have the width it was queued for:
        // overwriting it would make the relayout believe they are up to date.
        if (!view->relayoutQueued) {
            const PreviewStyle style = styleFor(browser);
            view->layoutWidth        = style.maxWidth;
            view->layoutDpr          = style.dpr;
            view->layoutDark         = style.dark;
        }
    }

    m_mutating = true;
    QTextCursor c(doc);
    c.setPosition(position);
    const QTextCharFormat separator = separatorFormat();
    c.insertText(QString(QChar::LineSeparator), separator);

    QTextImageFormat image;
    image.setName(url.toString());
    image.setWidth(size.width());
    image.setHeight(size.height() + kGapTop + kGapBottom);
    c.insertImage(image);
    if (!c.atBlockEnd())
        c.insertText(QString(QChar::LineSeparator), separator);
    m_mutating = false;

    if (bottom)
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QStringLiteral("[test] preview inserted for ") + key);
#endif
    requestSnapshot(browser, QStringLiteral("preview inserted for ") + key);
}

void ChatIntegration::refreshPreview(QTextBrowser* browser, const QString& key, bool pixelsOnly)
{
    View* view = viewFor(browser);
    if (!view)
        return;
    ensurePositions(*view);
    if (!albums::isObjectId(key))
        refreshAlbumsOf(browser, key, pixelsOnly); // 2.2 album: the grids that show it are drawn again too
    const auto found = view->positionsByKey.constFind(key);
    if (found == view->positionsByKey.constEnd())
        return;
    const QVector<int> positions = found.value();

    if (pixelsOnly && !onScreen(browser, *view, key)) {
        view->stale.insert(key); // redrawn by updateVisibleKeys once it can be seen again
        return;
    }

    QSize        size;
    const QImage img = renderFor(browser, key, &size);
    if (img.isNull())
        return;

    QTextDocument* doc = browser->document();
    doc->addResource(QTextDocument::ImageResource, QUrl(kScheme + key), paddedPixmap(img));
    view->resourced.insert(key);
    view->stale.remove(key);

    if (view->formatSizes.value(key) != size) {
        // The layout size changed (plain links once the picture is known, chat resized, ...).
        const bool    bottom = isAtBottom(browser);
        const QString name   = kScheme + key;
        // 2.2 reactions: a preview wholly above the visible part that grows or shrinks (a reaction row
        // appearing or going) must not move what the user is reading.
        const QSize oldSize = view->formatSizes.value(key);
        bool        above   = !bottom && oldSize.isValid();
        for (int pos : positions) {
            const QRectF r = previewRect(browser, pos, QSizeF(oldSize));
            above          = above && r.isValid() && r.bottom() <= 0;
        }
        m_mutating           = true;
        for (int pos : positions) {
            QTextCursor c(doc);
            c.setPosition(pos);
            c.setPosition(pos + 1, QTextCursor::KeepAnchor);
            QTextImageFormat f = c.charFormat().toImageFormat();
            if (f.name() != name)
                continue;
            f.setWidth(size.width());
            f.setHeight(size.height() + kGapTop + kGapBottom);
            c.setCharFormat(f);
        }
        m_mutating = false;
        if (View* v = viewFor(browser))
            v->formatSizes.insert(key, size);
        browser->viewport()->update();
        if (bottom) {
            browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
        } else if (above) { // 2.2 reactions: scroll anchoring
            doc->documentLayout()->documentSize(); // lays out the change, so the range is current
            QScrollBar* bar = browser->verticalScrollBar();
            bar->setValue(bar->value() + positions.size() * (size.height() - oldSize.height()));
        }
        scheduleVisibilityUpdate();
    } else {
        // Cheap path: same layout, only the pixels of the preview's rect change.
        for (int pos : positions) {
            const QRectF r = previewRect(browser, pos, QSizeF(size));
            if (r.isValid())
                browser->viewport()->update(r.toAlignedRect().adjusted(-2, -2, 2, 2));
            else
                browser->viewport()->update();
        }
    }
    requestSnapshot(browser, (pixelsOnly ? QStringLiteral("frame of ") : QStringLiteral("preview refreshed for ")) + key);
}

void ChatIntegration::refreshAll()
{
    const bool previewsOn = Settings::instance().inlinePreviews;
    if (m_media) {
        m_media->settingsChanged();
        if (!previewsOn)
            m_media->stopAll();
    }

    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        QTextBrowser* b = it->browser;
        if (!b)
            continue;
        if (!previewsOn) {
            // Remove our previews (and the line break we added before each one).
            QTextDocument* doc = b->document();
            QVector<int>   positions;
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                for (auto fit = block.begin(); !fit.atEnd(); ++fit) {
                    const QTextFragment f = fit.fragment();
                    if (f.isValid() && !previewKeyOf(f.charFormat()).isEmpty())
                        for (int i = 0; i < f.length(); ++i)
                            positions.append(f.position() + i);
                }
            }
            // Back to front, so earlier positions stay valid. The line break insertPreview put after
            // a preview (text followed the link) goes too, otherwise that text would stay on a line
            // of its own and every off/on toggle would add another empty line. The one before it
            // is always ours.
            m_mutating = true;
            for (int i = positions.size() - 1; i >= 0; --i) {
                const int   pos   = positions[i];
                const int   from  = pos > 0 && doc->characterAt(pos - 1) == QChar::LineSeparator ? pos - 1 : pos;
                const int   to    = isOurSeparator(doc, pos + 1) ? pos + 2 : pos + 1;
                QTextCursor c(doc);
                c.setPosition(from);
                c.setPosition(to, QTextCursor::KeepAnchor);
                c.removeSelectedText();
            }
            m_mutating = false;
            it->resourced.clear();
            it->stale.clear();
            scan(b); // notes stay hidden
            continue;
        }
        scan(b);
        if (View* view = viewFor(b)) {
            ensurePositions(*view);
            const PreviewStyle style = styleFor(b);
            view->layoutWidth        = style.maxWidth;
            view->layoutDpr          = style.dpr;
            view->layoutDark         = style.dark;
            const QStringList keys   = view->positionsByKey.keys();
            for (const QString& key : keys)
                refreshPreview(b, key, false);
        }
    }
    scheduleVisibilityUpdate();
}

void ChatIntegration::onEntryChanged(const QString& key)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser)
            refreshPreview(it->browser, key, false);
    }
}

void ChatIntegration::onFrameChanged(const QString& key)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser)
            refreshPreview(it->browser, key, true);
    }
}

void ChatIntegration::scheduleRelayout(QTextBrowser* browser)
{
    View* view = viewFor(browser);
    if (!view || view->relayoutQueued)
        return;
    view->relayoutQueued = true;
    QPointer<QTextBrowser> guard(browser);
    singleShotOwned(200, this, [this, guard] {
        View* v = guard ? viewFor(guard.data()) : nullptr;
        if (!v)
            return;
        v->relayoutQueued = false;
        // A new width re-lays the previews out; a new device pixel ratio (the window moved to
        // another monitor) or a theme switch re-renders them (sharp, in the chat's new colours).
        const PreviewStyle style = styleFor(guard.data());
        if (style.maxWidth == v->layoutWidth && style.dpr == v->layoutDpr && style.dark == v->layoutDark)
            return;
        v->layoutWidth = style.maxWidth;
        v->layoutDpr   = style.dpr;
        v->layoutDark  = style.dark;
        ensurePositions(*v);
        const QStringList keys = v->positionsByKey.keys();
        for (const QString& key : keys)
            refreshPreview(guard.data(), key, false);
        scheduleVisibilityUpdate();
    });
}

// ============================================================================================
// Preview geometry
// ============================================================================================

void ChatIntegration::ensurePositions(View& view)
{
    if (view.positionsValid || !view.browser)
        return;
    view.previews.clear();
    view.positionsByKey.clear();
    view.formatSizes.clear();

    QTextDocument* doc = view.browser->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            const QTextCharFormat cf  = f.charFormat();
            const QString         key = previewKeyOf(cf);
            if (key.isEmpty())
                continue;
            for (int i = 0; i < f.length(); ++i) {
                view.previews.append({f.position() + i, key});
                view.positionsByKey[key].append(f.position() + i);
            }
            view.formatSizes.insert(key, contentSizeOf(cf.toImageFormat()).toSize());
        }
    }
    indexAlbums(view); // 2.2 album
    view.positionsValid = true;
}

// Viewport rect of the content of the preview object at position; size is its content size
// (contentSizeOf / View::formatSizes), never the padded format size.
QRectF ChatIntegration::previewRect(QTextBrowser* browser, int position, const QSizeF& size) const
{
    QTextDocument*   doc   = browser->document();
    const QTextBlock block = doc->findBlock(position);
    if (!block.isValid())
        return {};
    const QTextLayout* layout = block.layout();
    if (!layout)
        return {};
    const int       rel  = position - block.position();
    const QTextLine line = layout->lineForTextPosition(rel);
    if (!line.isValid())
        return {};

    const QRectF blockRect = doc->documentLayout()->blockBoundingRect(block);
    const qreal  x1        = line.cursorToX(rel);
    const qreal  x2        = line.cursorToX(rel + 1);
    const qreal  left      = qAbs(x2 - x1) > 0.5 ? qMin(x1, x2) : x1;
    // Inline objects sit on the baseline; the content starts below the transparent top gap.
    const qreal objectHeight = size.height() + kGapTop + kGapBottom;
    const qreal top          = qMax(line.y(), line.y() + line.ascent() - objectHeight) + kGapTop;
    return QRectF(blockRect.left() + left - browser->horizontalScrollBar()->value(), blockRect.top() + top - browser->verticalScrollBar()->value(),
                  size.width(), size.height());
}

ChatIntegration::Hit ChatIntegration::previewAt(QTextBrowser* browser, const QPoint& viewportPos) const
{
    QTextDocument* doc = browser->document();
    const QPointF  docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return {};

    const int last = doc->characterCount() - 1;
    for (int p : {hit, hit - 1}) {
        if (p < 0 || p + 1 > last)
            continue;
        QTextCursor c(doc);
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat cf  = c.charFormat();
        const QString         key = previewKeyOf(cf);
        if (key.isEmpty())
            continue;
        // The rect of what is drawn: the gaps around it belong to the chat text, not the preview.
        QRectF r = previewRect(browser, p, contentSizeOf(cf.toImageFormat()));
        if (m_reactions) // 2.2 reactions: the row below the picture is ChatReactions'
            r = m_reactions->pictureRect(browser, key, r);
        if (r.contains(viewportPos))
            return albums::isObjectId(key) ? albumHit(browser, key, r, viewportPos) : Hit{key, r, key}; // 2.2 album: per tile
    }
    return {};
}

void ChatIntegration::scheduleVisibilityUpdate()
{
    if (m_visibilityQueued)
        return;
    m_visibilityQueued = true;
    singleShotOwned(30, this, [this] { updateVisibleKeys(); });
}

bool ChatIntegration::onScreen(QTextBrowser* browser, View& view, const QString& key)
{
    if (!browser->isVisible() || browser->window()->isMinimized())
        return false;
    ensurePositions(view);
    const QRectF viewport(browser->viewport()->rect());
    const QSizeF size(view.formatSizes.value(key));
    for (int pos : view.positionsByKey.value(key)) {
        if (previewRect(browser, pos, size).intersects(viewport))
            return true;
    }
    return false;
}

void ChatIntegration::updateVisibleKeys()
{
    m_visibilityQueued = false;
    if (!m_media)
        return;

    QSet<QString> keys;    // on screen in a visible chat
    QSet<QString> present; // in any chat document, seen or not
    bool          settled = true;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        View&         view = it.value();
        QTextBrowser* b    = view.browser;
        if (!b)
            continue;
        ensurePositions(view);
        // A document that was just swapped or changed gets its previews back with the next scan.
        if (view.scanQueued || view.document != b->document()) {
            settled = false;
        } else {
            for (auto pit = view.positionsByKey.constBegin(); pit != view.positionsByKey.constEnd(); ++pit)
                present.insert(pit.key());
        }
        if (!b->isVisible() || b->window()->isMinimized())
            continue;
        // Moving to a monitor with another scale, or a theme switch that sent no palette or style
        // event, is noticed here (this runs every 300 ms).
        if (!view.previews.isEmpty() && view.layoutDpr > 0 && !view.relayoutQueued) {
            const PreviewStyle style = styleFor(b);
            if (style.dpr != view.layoutDpr || style.dark != view.layoutDark)
                scheduleRelayout(b);
        }
        const QRectF viewport(b->viewport()->rect());
        for (const PreviewPos& pp : qAsConst(view.previews)) {
            const QRectF r = previewRect(b, pp.position, QSizeF(view.formatSizes.value(pp.key)));
            if (!r.intersects(viewport))
                continue;
            if (albums::isObjectId(pp.key)) // 2.2 album: its tiles on screen (the grid, not its reaction row)
                albumVisibleKeys(b, pp.key, m_reactions ? m_reactions->pictureRect(b, pp.key, r) : r, viewport, &keys);
            else
                keys.insert(pp.key);
        }
        if (!view.stale.isEmpty()) {
            const QSet<QString> stale = view.stale;
            view.stale.clear();
            for (const QString& key : stale)
                refreshPreview(b, key, true);
        }
    }
    m_media->setVisibleKeys(keys);
    if (settled)
        m_media->setPresentKeys(present);
}

// ============================================================================================
// Input: hover, clicks, context menu, drag & drop, paste
// ============================================================================================

void ChatIntegration::updateCursor(QTextBrowser* browser, bool overPreview, const QPoint& viewportPos, bool actionable)
{
    QWidget* vp = browser->viewport();
    if (overPreview) {
        if (m_cursorOwner != vp) {
            if (m_cursorOwner)
                m_cursorOwner->setCursor(m_savedCursor);
            m_cursorOwner = vp;
            m_savedCursor = vp->cursor().shape() == Qt::PointingHandCursor ? Qt::ArrowCursor : vp->cursor().shape();
        }
        // A preview a click can't help (file deleted, password-protected channel) is not a button.
        const Qt::CursorShape shape = actionable ? Qt::PointingHandCursor : Qt::ArrowCursor;
        if (vp->cursor().shape() != shape)
            vp->setCursor(shape);
        return;
    }
    if (m_cursorOwner != vp)
        return;
    m_cursorOwner = nullptr;
    // After QTextBrowser handled the event: links keep their own pointer cursor.
    QTimer::singleShot(0, this, [vp = QPointer<QWidget>(vp), b = QPointer<QTextBrowser>(browser), viewportPos, shape = m_savedCursor] {
        if (vp && b && b->anchorAt(viewportPos).isEmpty())
            vp->setCursor(shape);
    });
}

// Redraws key with the current hover / pressed look (renderFor reads m_hoverKey and m_pressedKey).
// Videos already repaint on hover through InlineMediaController; a press needs it for them too.
void ChatIntegration::repaintPointerState(QTextBrowser* browser, const QString& key, bool includeVideos)
{
    if (key.isEmpty() || !m_media)
        return;
    const InlineMediaController::Mode mode = m_media->mode(key);
    const bool player = mode == InlineMediaController::Mode::Video || mode == InlineMediaController::Mode::Audio; // 2.2 audio
    if (mode == InlineMediaController::Mode::Still || (includeVideos && player))
        refreshPreview(browser, key, true);
}

// The tooltip of the preview under viewportPos, as rich text (empty: none). Video controls get
// their name ("Pause", "Mute", the time under the pointer on the seek bar), and area is narrowed to
// that control so the tip goes away when the pointer leaves it. Everything else gets the file's
// name and size, plus the full status where the preview shortens it or explains a failure.
// Built from i18n::t / arg() only: QToolTip's label outlives the plugin DLL.
QString ChatIntegration::toolTipText(QTextBrowser* browser, const Hit& hit, const QPointF& viewportPos, QRect* area) const
{
    if (hit.tile >= 0)
        return albumToolTip(hit, area); // 2.2 album
    const MediaEntry* e = m_core->entry(hit.key);
    if (!e)
        return {};
    *area = hit.rect.toAlignedRect();
    if (m_core->isSpoilerHidden(hit.key))
        return spoilerToolTip(); // 2.2 spoiler: no name, size or state

    QString    detail;
    const bool audioCard = m_media && m_media->mode(hit.key) == InlineMediaController::Mode::Audio; // 2.2 audio
    if (audioCard) {
        const PlaybackOverlay o      = m_media->overlay(hit.key);
        const PreviewStyle    style  = styleFor(browser);
        const QSize           size   = hit.rect.size().toSize();
        const QPointF         local  = viewportPos - hit.rect.topLeft();
        const VideoZone       zone   = audioZoneAt(*e, o, style, size, local);
        const QString         button = audioZoneToolTip(*e, o, zone, audioSeekFractionAt(*e, o, style, size, local));
        if (!button.isEmpty()) {
            if (!o.busy)
                *area = audioZoneRect(*e, o, style, size, local).translated(hit.rect.topLeft()).toAlignedRect();
            return button;
        }
        detail = audioToolTipDetail(*e, o);
    } else if (e->kind == MediaKind::Video && m_media) {
        const PlaybackOverlay o = m_media->overlay(hit.key);
        if (o.busy) {
            if (isCheckingShown(*e)) // 2.2 sha
                return i18n::t("%1 Click to cancel autoplay.").arg(checkingText(*e));
            if (e->state == MediaState::Downloading)
                return i18n::t("Downloading… Click to cancel autoplay.");
            if (e->state == MediaState::Ready)
                return i18n::t("Opening… Click to cancel autoplay.");
            return i18n::t("Waiting to download… Click to cancel autoplay.");
        }
        if (o.controlsVisible && videoStarted(o, !m_media->frame(hit.key).isNull())) {
            const QSize   size   = hit.rect.size().toSize();
            const QPointF local  = viewportPos - hit.rect.topLeft();
            const bool    center = !o.playing;
            *area                = videoZoneRect(size, local, center).translated(hit.rect.topLeft()).toAlignedRect();
            switch (videoZoneAt(size, local, center)) {
            case VideoZone::PlayPause:
                return o.playing ? i18n::t("Pause") : o.ended ? i18n::t("Replay") : i18n::t("Play");
            case VideoZone::Mute:
                return o.muted ? i18n::t("Unmute") : i18n::t("Mute");
            case VideoZone::Expand:
                return i18n::t("Open in viewer");
            case VideoZone::Seek:
                return o.durationMs > 0 ? formatDuration(qRound64(seekFractionAt(size, local) * static_cast<double>(o.durationMs))) : QString();
            case VideoZone::None:
            case VideoZone::Body:
                break;
            }
            return {};
        }
        if (o.playing)
            return {}; // the picture itself: no tip over a playing video
        if (o.externalOnly)
            detail = i18n::t("Windows can't play this video here, so it opens in your default app.");
    }

    if (audioCard) {
        // 2.2 audio: detail set above (a click on the card plays it: no "Click to open")
    } else if (e->state == MediaState::Failed) {
        detail = e->errorText.isEmpty() ? downloadErrorText(e->error) : e->errorText;
        if (isRetryableDownload(*e))
            detail += QLatin1Char(' ') + i18n::t("Click to retry.");
    } else if (e->kind != MediaKind::Video) {
        // Pictures shown inline need no status; cards (and pictures still on their way) get theirs
        // in full, since the card may have shortened it.
        const PreviewStyle style         = styleFor(browser);
        const bool         picture       = isPreviewableImage(e->kind);
        const bool         cannotPreview = picture && e->state == MediaState::Ready
                                   && m_core->still(hit.key, stillPixelsFor(*e, style)).source == MediaStill::None;
        const bool revealOnly = !picture && e->state == MediaState::Ready && m_core->isUnsafeToOpen(hit.key);
        if (revealOnly)
            detail = i18n::t("Programs and scripts from chat aren't opened directly. Click to show the file in its folder.");
        else if (!picture || e->state != MediaState::Ready || cannotPreview)
            detail = previewStatusText(*e, cannotPreview);
    }
    // 2.2 data saver: why it waits for a click.
    if (e->state == MediaState::Idle && e->heldByDataSaver && !e->tooLargeForAuto)
        detail = i18n::t("Data saver is on for this server. Click to load.");

    // The name comes from someone else's chat link: shown as plain text (escaped, since a tool tip
    // would interpret markup), without bidi/control characters.
    QString name = isVoiceCard(*e) ? voiceToolTipName(*e) : displayNameFor(e->link); // 2.2 voice: "Voice message · 0:12"
    if (e->link.size)
        name += QStringLiteral(" · ") + formatSize(e->link.size);
    QString text = QStringLiteral("<div style='white-space:pre'>%1</div>").arg(name.toHtmlEscaped());
    if (!detail.isEmpty())
        text += QStringLiteral("<div>%1</div>").arg(detail.toHtmlEscaped());
    return text;
}

void ChatIntegration::activate(const Hit& hit, const QPointF& viewportPos, bool controlsVisible)
{
    if (hit.tile >= 0) {
        activateAlbumTile(hit); // 2.2 album: tiles open the viewer, videos never play inside one
        return;
    }
    const MediaEntry* e = m_core->entry(hit.key);
    if (!e)
        return;
    const QString key = hit.key;
    if (m_core->isSpoilerHidden(key)) { // 2.2 spoiler: the first click only reveals (no download, playback or viewer)
        revealSpoiler(key);
        return;
    }

    // 2.2 audio: the card plays inline; its controls are always shown, so the zone is taken as is.
    if (m_media && m_media->mode(key) == InlineMediaController::Mode::Audio) {
        QTextBrowser* browser = m_pressBrowser ? m_pressBrowser.data() : m_lastBrowser.data(); // the chat the click was in
        if (!browser)
            return;
        const PlaybackOverlay o     = m_media->overlay(key);
        const PreviewStyle    style = styleFor(browser);
        const QSize           size  = hit.rect.size().toSize();
        const QPointF         local = viewportPos - hit.rect.topLeft();
        m_media->click(key, audioZoneAt(*e, o, style, size, local), audioSeekFractionAt(*e, o, style, size, local));
        return;
    }

    if (e->kind == MediaKind::Video) {
        if (!m_media)
            return;
        const QSize   size  = hit.rect.size().toSize();
        const QPointF local = viewportPos - hit.rect.topLeft();
        // Hidden controls cannot be aimed at: a click anywhere on the picture just toggles playback
        // (also on the seek bar's band). controlsVisible is the state when the button went down.
        VideoZone zone = videoZoneAt(size, local, !m_media->overlay(key).playing);
        if (!controlsVisible)
            zone = VideoZone::Body;
        m_media->click(key, zone, seekFractionAt(size, local));
        return;
    }

    if (!isPreviewActionable(*e))
        return; // deleted from the server / password-protected channel: retrying can't help

    switch (e->state) {
    case MediaState::Ready:
        if (isPreviewableImage(e->kind))
            QTimer::singleShot(0, this, [this, key] { openViewer(key); }); // outside the event filter
        else
            m_core->openExternally(key);
        break;
    case MediaState::Failed:
        // "Click to retry": pictures show up inline again, files open once they are there.
        if (isPreviewableImage(e->kind))
            m_core->retry(key);
        else
            m_core->download(key, true);
        break;
    case MediaState::Idle:
    case MediaState::Queued:
    case MediaState::Downloading:
        m_core->download(key, true); // opens (viewer / default app) when finished
        break;
    }
}

void ChatIntegration::openKey(const QString& key)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return;
    if (isMediaKind(e->kind))
        openViewer(key); // the viewer downloads what is missing
    else if (e->state == MediaState::Ready)
        m_core->openExternally(key);
    else
        m_core->download(key, true);
}

void ChatIntegration::onOpenRequested(const QString& key)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return;
    if (isMediaKind(e->kind))
        openViewer(key);
    else
        m_core->openExternally(key);
}

void ChatIntegration::openViewer(const QString& key)
{
    if (!m_core->entry(key))
        return;

    auto holds = [this, &key](QTextBrowser* b) {
        View* v = viewFor(b);
        if (!v || !v->browser)
            return false;
        ensurePositions(*v);
        return v->positionsByKey.contains(key) || v->albumsByKey.contains(key); // 2.2 album
    };
    QTextBrowser* source = nullptr;
    if (holds(m_lastBrowser.data()))
        source = m_lastBrowser.data();
    else if (holds(visibleChatBrowser()))
        source = visibleChatBrowser();
    else {
        for (auto it = m_views.begin(); it != m_views.end() && !source; ++it) {
            if (holds(it->browser.data()))
                source = it->browser.data();
        }
    }

    // Gallery: every previewable media of that chat, in document order.
    QStringList keys;
    if (View* v = viewFor(source)) {
        QSet<QString> seen;
        for (const PreviewPos& pp : qAsConst(v->previews)) {
            // 2.2 album: a grid adds its items in grid order, the ones behind "+N" too.
            const QStringList shown = albums::isObjectId(pp.key) ? albumKeysOf(pp.key) : QStringList{pp.key};
            for (const QString& k : shown) {
                if (k.isEmpty() || seen.contains(k))
                    continue;
                seen.insert(k);
                const MediaEntry* e = m_core->entry(k);
                if (e && isMediaKind(e->kind))
                    keys.append(k);
            }
        }
    }
    int index = keys.indexOf(key);
    if (index < 0) {
        keys  = QStringList{key};
        index = 0;
    }
    if (!m_media) {
        MediaViewer::open(m_core, keys, index);
        return;
    }
    // 2.2 audio: an audio card keeps playing while pictures are viewed; the viewer's own playback
    // pauses it (playbackStarted below).
    m_media->pauseVideos();
    // The viewer starts with the chat's session mute, and the inline players follow what the user
    // changes there. Only one thing plays at a time: playback starting in the viewer pauses the inline
    // players, and an inline video starting pauses the viewer (see start()).
    if (MediaViewer* viewer = MediaViewer::open(m_core, keys, index, m_media->isMuted())) {
        m_viewer = viewer;
        connect(viewer, &MediaViewer::mutedChanged, m_media, &InlineMediaController::setMuted, Qt::UniqueConnection);
        connect(viewer, &MediaViewer::playbackStarted, m_media, &InlineMediaController::pauseAll, Qt::UniqueConnection);
        connect(viewer, &MediaViewer::playbackStarted, this, &ChatIntegration::playbackStarted, Qt::UniqueConnection); // 2.2 voice
    }
}

bool ChatIntegration::copyImage(const QString& key, QString* feedback)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e || e->state != MediaState::Ready || !isPreviewableImage(e->kind))
        return false;
    QImageReader reader(e->localPath);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaxCopyPixels) {
        *feedback = i18n::t("This image is too large to copy. Open it in your default app and copy it from there.");
        return false;
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        *feedback = i18n::t("Couldn't copy this image. Use Save as… instead.");
        return false;
    }
    QGuiApplication::clipboard()->setImage(image);
    *feedback = i18n::t("Image copied");
    return true;
}

// Copy and save confirmations next to the pointer. Not a QStringLiteral: QToolTip's label is a
// process-wide widget that outlives the plugin.
void ChatIntegration::showFeedback(QWidget* widget, const QString& text, bool error) const
{
    if (text.isEmpty())
        return;
    QToolTip::showText(QCursor::pos(), text, widget, QRect(), error ? qMax(4000, ui::notificationDurationMs()) : 1800);
}

void ChatIntegration::showContextMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos, const QString& album)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return;
    if (m_core->isSpoilerHidden(key)) { // 2.2 spoiler: "Reveal spoiler", nothing that shows the content
        showSpoilerMenu(browser, key, globalPos, album);
        return;
    }
    const bool albumTile = !album.isEmpty(); // 2.2 album
    const bool ready = e->state == MediaState::Ready;
    const bool media = isMediaKind(e->kind);
    // Deleted from the server or in a password-protected channel: nothing here can get the file
    // (a click on the preview does nothing either).
    const bool gone = !isPreviewActionable(*e);
    // Programs and scripts from the chat are never run: "Open" would only show them in their folder.
    const bool unsafe = !media && m_core->isUnsafeToOpen(key);

    auto* menu = new QMenu(browser);
    menu->setObjectName(QStringLiteral("tsmediaPreviewMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    QPointer<QTextBrowser> guard(browser);
    auto viewport = [guard]() -> QWidget* { return guard ? guard->viewport() : nullptr; };
    QAction* primary  = nullptr; // what a click on the preview does; Windows shows it in bold
    bool     external = false;   // a video Windows can't play inline: its primary item opens the default app

    // The preview's own state: playback, or getting the file.
    if (gone) {
        menu->addAction(downloadErrorTitle(e->error))->setEnabled(false);
    } else {
        if (e->kind == MediaKind::Video && m_media && !albumTile) { // 2.2 album: tiles open in the viewer
            const PlaybackOverlay o      = m_media->overlay(key);
            const bool            active = o.playing || o.ended || o.positionMs > 0;
            // The click hands such a video to the default app, as its preview says ("Opens in default app").
            external = o.externalOnly && !o.playing;
            primary  = menu->addAction(o.playing ? i18n::t("&Pause") : external ? i18n::t("Open in &default app") : i18n::t("&Play"), this, [this, key] {
                if (m_media)
                    m_media->click(key, VideoZone::PlayPause, 0.0);
            });
            if (active) {
                menu->addAction(o.muted ? i18n::t("Un&mute") : i18n::t("&Mute"), this, [this, key] {
                    if (!m_media)
                        return;
                    // The player may have been closed meanwhile; Mute must never start playback.
                    const PlaybackOverlay now = m_media->overlay(key);
                    if (now.playing || now.ended || now.positionMs > 0)
                        m_media->click(key, VideoZone::Mute, 0.0);
                });
            }
        } else if (m_media && m_media->mode(key) == InlineMediaController::Mode::Audio) { // 2.2 audio
            const PlaybackOverlay o = m_media->overlay(key);
            external                = o.externalOnly && !o.playing;
            primary = menu->addAction(o.playing ? i18n::t("&Pause") : external ? i18n::t("Open in &default app") : o.ended ? i18n::t("&Replay") : i18n::t("&Play"),
                                      this, [this, key] {
                                          if (m_media)
                                              m_media->click(key, VideoZone::PlayPause, 0.0);
                                      });
        }
        QAction* fetch = nullptr;
        if (e->state == MediaState::Idle)
            fetch = menu->addAction(i18n::t("&Download"), this, [this, key] { m_core->download(key, false); });
        else if (isRetryableDownload(*e))
            fetch = menu->addAction(i18n::t("&Retry download"), this, [this, key] { m_core->retry(key); });
        if (!primary)
            primary = fetch;
    }
    menu->addSeparator(); // separators at the start or end of a menu are not shown

    // Opening it.
    if (!gone && !unsafe) {
        QAction* open = menu->addAction(i18n::t("&Open"), this, [this, key] { openKey(key); });
        if (!primary || albumTile) // 2.2 album: what a click on a tile does
            primary = open;
    }
    if (media && ready && !external)
        menu->addAction(i18n::t("Open &with default app"), this, [this, key] { m_core->openExternally(key); });
    if (ready) {
        QAction* reveal = menu->addAction(i18n::t("Show in &folder"), this, [this, key] { m_core->revealInFolder(key); });
        if (!primary && unsafe)
            primary = reveal;
    }
    menu->addSeparator();

    // Keeping or passing it on. No Ctrl+C / Ctrl+S hints: in the chat those keys act on the text selection.
    if (ready) {
        menu->addAction(i18n::t("&Save as…"), this, [this, key, guard, viewport] {
            const QString saved = m_core->saveAs(key, guard ? guard->window() : mainWindow());
            if (!saved.isEmpty())
                showFeedback(viewport(), ui::savedToText(saved), false);
        });
        // 2.2 drag-out: the keyboard / single-pointer way to put the file into a folder or an app.
        menu->addAction(i18n::t("Copy fil&e"), this, [this, key, viewport] {
            QString    feedback;
            const bool ok = filedrag::copyToClipboard(m_core, key, &feedback); // refuses a covered spoiler
            showFeedback(viewport(), feedback, !ok);
        });
    }
    if (ready && isPreviewableImage(e->kind)) {
        menu->addAction(i18n::t("&Copy image"), this, [this, key, viewport] {
            QString    feedback;
            const bool ok = copyImage(key, &feedback);
            showFeedback(viewport(), feedback, !ok);
        });
    }
    menu->addAction(i18n::t("Copy &link"), this, [this, key, viewport] {
        if (const MediaEntry* entry = m_core->entry(key)) {
            QGuiApplication::clipboard()->setText(entry->link.toUrl());
            showFeedback(viewport(), i18n::t("Link copied"), false);
        }
    });
    // 2.2 sha: both checksums and the file, for whoever looks into it (the sender, a server admin).
    if (e->state == MediaState::Failed && e->error == MediaError::Mismatch) {
        menu->addAction(i18n::t("Copy &details"), this, [this, key, viewport] {
            if (const MediaEntry* entry = m_core->entry(key)) {
                QGuiApplication::clipboard()->setText(fileverify::mismatchDetails(entry->link.sha256, entry->check.received, entry->link.remoteFile()));
                showFeedback(viewport(), i18n::t("Details copied"), false);
            }
        });
    }
    addHideSpoilerAction(menu, key); // 2.2 spoiler
    if (m_reactions) // 2.2 reactions: "Add reaction", the way without hover (an album's tile: the album's)
        m_reactions->addMenu(menu, browser, albumTile ? album : key);

    if (primary)
        menu->setDefaultAction(primary);
    menu->popup(globalPos);
}

// ============================================================================================
// Sending: drag & drop, paste, file picker
// ============================================================================================

namespace {

QString dot()
{
    return QStringLiteral(" · ");
}

QFont scaledFont(const QFont& base, qreal factor)
{
    QFont font(base);
    if (base.pixelSize() > 0)
        font.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        font.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    return font;
}

// An arrow going up out of a tray, in the stroke style of the other plugin glyphs.
void drawUploadGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.setPen(QPen(color, s * 0.08, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(cx, box.top() + s * 0.12), QPointF(cx, box.top() + s * 0.64));
    QPainterPath head;
    head.moveTo(cx - s * 0.22, box.top() + s * 0.34);
    head.lineTo(cx, box.top() + s * 0.12);
    head.lineTo(cx + s * 0.22, box.top() + s * 0.34);
    p.drawPath(head);
    QPainterPath tray;
    tray.moveTo(box.left() + s * 0.14, box.top() + s * 0.62);
    tray.lineTo(box.left() + s * 0.14, box.top() + s * 0.86);
    tray.lineTo(box.right() - s * 0.14, box.top() + s * 0.86);
    tray.lineTo(box.right() - s * 0.14, box.top() + s * 0.62);
    p.drawPath(tray);
}

void drawBlockedGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.setPen(QPen(color, s * 0.08));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(box.adjusted(s * 0.08, s * 0.08, -s * 0.08, -s * 0.08));
    p.setPen(QPen(color, s * 0.09, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx, box.top() + s * 0.30), QPointF(cx, box.top() + s * 0.54));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(cx, box.top() + s * 0.70), s * 0.055, s * 0.055);
}

// Shown over a chat while files are dragged onto it: it says where they go, what the drop does (2.2:
// open the send window or send right away) and how to do the other, or why nothing can be sent there.
// Takes no mouse or drag events itself.
class DropOverlay : public QWidget
{
  public:
    explicit DropOverlay(QWidget* parent)
        : QWidget(parent)
    {
        // Swept at plugin shutdown like our other widgets. fromLatin1: it lives in TeamSpeak's chat widget.
        setObjectName(QString::fromLatin1("tsmediaDropOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setLayoutDirection(Qt::LeftToRight);
    }

    void setContent(bool dark, bool blocked, const QString& title, const QString& subtitle, const QString& hint)
    {
        m_dark     = dark;
        m_blocked  = blocked;
        m_title    = title;
        m_subtitle = subtitle;
        m_hint     = hint;
        update();
    }

    QFont titleFont() const
    {
        QFont font = scaledFont(this->font(), 1.25);
        font.setWeight(QFont::DemiBold);
        return font;
    }

    qreal textWidth() const { return qMax(0.0, width() - 2.0 * kInset - 24.0); }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // Opaque enough that the text keeps 4.5:1 over any picture in the chat.
        p.fillRect(rect(), m_dark ? QColor(30, 31, 34, 225) : QColor(255, 255, 255, 230));
        const QColor accent = m_blocked ? (m_dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d))
                                        : (m_dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4));
        const QColor text   = m_dark ? QColor(0xf2, 0xf3, 0xf5) : QColor(0x06, 0x06, 0x07);
        const QColor muted  = m_dark ? QColor(0xb5, 0xba, 0xc1) : QColor(0x5c, 0x5e, 0x66);

        const QRectF frame = QRectF(rect()).adjusted(kInset, kInset, -kInset, -kInset);
        if (frame.width() < 48 || frame.height() < 48)
            return;
        QPen border(accent, 2.0, Qt::DashLine);
        border.setDashPattern({4.0, 3.0});
        p.setPen(border);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 8, 8);

        const QFont         bodyFont = font();
        const QFont         headFont = titleFont();
        const QFont         hintFont = scaledFont(bodyFont, 0.9);
        const QFontMetricsF tfm(headFont), bfm(bodyFont), hfm(hintFont);
        const qreal         textW    = textWidth();
        const qreal         glyph    = 28.0;
        const int           wrap     = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
        // The subtitle (a target name or a reason) may take two lines; everything is centred as a block.
        const qreal subH  = qMin(bfm.boundingRect(QRectF(0, 0, textW, 1000), wrap, m_subtitle).height(), bfm.lineSpacing() * 2.0 + 1.0);
        // 2.2 compose: the hint (what the drop does, Ctrl and Shift) may take two lines as well.
        const qreal hintH = m_hint.isEmpty() ? 0.0 : qMin(hfm.boundingRect(QRectF(0, 0, textW, 1000), wrap, m_hint).height(), hfm.lineSpacing() * 2.0 + 1.0);
        const qreal total = glyph + 10 + tfm.height() + 4 + subH + (m_hint.isEmpty() ? 0.0 : 8 + hintH);
        const qreal left  = frame.left() + 12;
        qreal       y     = qMax(frame.top() + 8, frame.center().y() - total / 2);

        const QRectF glyphBox(frame.center().x() - glyph / 2, y, glyph, glyph);
        if (m_blocked)
            drawBlockedGlyph(p, glyphBox, accent);
        else
            drawUploadGlyph(p, glyphBox, accent);
        y += glyph + 10;
        p.setFont(headFont);
        p.setPen(text);
        p.drawText(QRectF(left, y, textW, tfm.height()), Qt::AlignCenter, tfm.elidedText(m_title, Qt::ElideRight, textW));
        y += tfm.height() + 4;
        p.setFont(bodyFont);
        p.setPen(muted);
        p.drawText(QRectF(left, y, textW, subH), wrap, m_subtitle);
        y += subH;
        if (!m_hint.isEmpty()) {
            y += 8;
            p.setFont(hintFont);
            p.drawText(QRectF(left, y, textW, hintH), wrap, m_hint);
        }
    }

  private:
    static constexpr qreal kInset = 12.0;

    bool    m_dark    = false;
    bool    m_blocked = false;
    QString m_title;
    QString m_subtitle;
    QString m_hint;
};

} // namespace

bool ChatIntegration::acceptsDrop(const QMimeData* mime) const
{
    if (!mime || filedrag::isOwn(mime)) // 2.2 drag-out: our own drags are never sent again
        return false;
    for (const QString& format : mime->formats()) {
        // Drags from TeamSpeak's own file browser keep their native behaviour.
        if (format.contains(QLatin1String("ts3"), Qt::CaseInsensitive))
            return false;
    }
    if (mime->hasUrls()) {
        const auto urls = mime->urls();
        if (urls.isEmpty())
            return false;
        for (const QUrl& url : urls) {
            if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile())
                return false;
        }
        return true;
    }
    return mime->hasImage();
}

void ChatIntegration::sendMime(const QMimeData* mime, const ChatTarget& target)
{
    if (mime->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : mime->urls())
            paths.append(url.toLocalFile());
        m_core->uploadFiles(paths, target);
    } else if (mime->hasImage()) {
        m_core->uploadImage(qvariant_cast<QImage>(mime->imageData()), target);
    }
}

ChatIntegration::Hit ChatIntegration::updateHoverAt(QTextBrowser* browser, const QPointF& viewportPos, bool moved, VideoZone* zoneOut)
{
    const QPoint      pos   = viewportPos.toPoint();
    const Hit         hit   = previewAt(browser, pos);
    const MediaEntry* entry   = hit.key.isEmpty() ? nullptr : m_core->entry(hit.key);
    const bool        covered = !hit.key.isEmpty() && m_core->isSpoilerHidden(hit.key); // 2.2 spoiler: no controls, always a button
    VideoZone         zone    = VideoZone::None;
    if (!hit.key.isEmpty()) {
        const InlineMediaController::Mode mode = m_media->mode(hit.key);
        if (covered || hit.tile >= 0) // 2.2 album: tiles have no controls
            zone = VideoZone::Body;
        else if (mode == InlineMediaController::Mode::Video)
            zone = videoZoneAt(hit.rect.size().toSize(), viewportPos - hit.rect.topLeft(), !m_media->overlay(hit.key).playing);
        else if (mode == InlineMediaController::Mode::Audio && entry) // 2.2 audio
            zone = audioZoneAt(*entry, m_media->overlay(hit.key), styleFor(browser), hit.rect.size().toSize(), viewportPos - hit.rect.topLeft());
        else
            zone = VideoZone::Body;
    }
    // 2.2 album: hover per tile, in the chat it is hovered in (a grid can be in several).
    if (hit.object != m_hoverObject || hit.tile != m_hoverTile || (!hit.object.isEmpty() && browser != m_hoverObjectIn)) {
        const QString                left   = m_hoverObject;
        const QPointer<QTextBrowser> leftIn = m_hoverObjectIn;
        m_hoverObject                       = hit.object;
        m_hoverTile                         = hit.tile;
        m_hoverObjectIn                     = hit.object.isEmpty() ? nullptr : browser;
        if (albums::isObjectId(left) && leftIn)
            refreshPreview(leftIn, left, true);
        if (albums::isObjectId(hit.object))
            refreshPreview(browser, hit.object, true);
    }
    const bool otherPreview = hit.key != m_hoverKey || (!hit.key.isEmpty() && browser != m_hoverBrowser);
    const bool changed      = otherPreview || zone != m_hoverZone;
    if (otherPreview) {
        // The look belongs to one chat: the same file shown in another one keeps its normal look.
        const QString                left   = m_hoverKey;
        const QPointer<QTextBrowser> leftIn = m_hoverBrowser;
        m_hoverKey                          = hit.key;
        m_hoverBrowser                      = hit.key.isEmpty() ? nullptr : browser;
        if (leftIn)
            repaintPointerState(leftIn, left, false);
        repaintPointerState(browser, hit.key, false);
    }
    m_hoverZone = zone;
    // A move keeps the video controls shown for a while; a scroll under a still pointer must not, or
    // they would stay over a video that moved away.
    if (moved || changed)
        m_media->hover(hit.key, zone);
    updateCursor(browser, !hit.key.isEmpty(), pos, !entry || covered || isPreviewActionable(*entry));
    if (zoneOut)
        *zoneOut = zone;
    return hit;
}

bool ChatIntegration::eventFilter(QObject* watched, QEvent* event)
{
    const Settings& s = Settings::instance();
    if (m_reactions && m_reactions->filterEvent(watched, event)) // 2.2 reactions: pills, add button
        return true;
    // 2.2 audio: the seek position under the pointer, on a video player or an audio card.
    auto seekFraction = [this](QTextBrowser* browser, const QString& key, const QSize& size, const QPointF& local) {
        if (m_media && m_media->mode(key) == InlineMediaController::Mode::Audio) {
            if (const MediaEntry* e = m_core->entry(key))
                return audioSeekFractionAt(*e, m_media->overlay(key), styleFor(browser), size, local);
        }
        return seekFractionAt(size, local);
    };

    switch (event->type()) {
    case QEvent::MouseMove: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser || !m_media)
            break;
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_seeking) {
            if ((me->buttons() & Qt::LeftButton) && m_pressBrowser == browser) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_lastSeekMs >= 60) {
                    m_lastSeekMs           = now;
                    const double fraction  = seekFraction(browser, m_pressedKey, m_pressRect.size().toSize(), me->localPos() - m_pressRect.topLeft());
                    m_media->click(m_pressedKey, VideoZone::Seek, fraction);
                    // The time being sought to follows the pointer (hidden again by the release).
                    const qint64 duration = m_media->overlay(m_pressedKey).durationMs;
                    if (duration > 0)
                        QToolTip::showText(me->globalPos(), formatDuration(qRound64(fraction * static_cast<double>(duration))), browser->viewport());
                }
                return true;
            }
            m_seeking = false;
        }
        // 2.2 drag-out: a press on a preview that moves far enough drags its file out of the chat (the
        // pending click is canceled). Not from video controls that were visible at the press, nor from an
        // audio card's play button or seek bar.
        // Synchronous, like QAbstractItemView::startDrag: Windows' drag loop starts while the button
        // is still down. Chat updates run inside it, so nothing from before is used afterwards.
        if (!m_pressedKey.isEmpty() && (me->buttons() & Qt::LeftButton) && m_pressBrowser == browser
            && (me->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            const QString key   = m_pressedKey;
            bool          drags = !m_core->isSpoilerHidden(key); // 2.2 spoiler: a covered file is never dragged out
            const InlineMediaController::Mode pressMode = m_media->mode(key);
            if (!drags) {
                // stays a click: the release reveals it
            } else if (m_pressControlsVisible && pressMode == InlineMediaController::Mode::Video) {
                const VideoZone zone = videoZoneAt(m_pressRect.size().toSize(), QPointF(m_pressPos) - m_pressRect.topLeft(), !m_media->overlay(key).playing);
                drags                = zone == VideoZone::Body || zone == VideoZone::None;
            } else if (pressMode == InlineMediaController::Mode::Audio) { // 2.2 audio
                const MediaEntry* pressed = m_core->entry(key);
                const VideoZone   zone    = pressed ? audioZoneAt(*pressed, m_media->overlay(key), styleFor(browser), m_pressRect.size().toSize(),
                                                                  QPointF(m_pressPos) - m_pressRect.topLeft())
                                                    : VideoZone::None;
                drags                     = zone == VideoZone::Body || zone == VideoZone::None;
            }
            if (drags) {
                const QString pressedObject = m_pressedObject; // 2.2 album: a tile drags its own file
                m_pressedKey.clear();
                m_pressedObject.clear();
                m_pressedTile = -1;
                if (albums::isObjectId(pressedObject))
                    refreshPreview(browser, pressedObject, true);
                else
                    repaintPointerState(browser, key, true);
                const QSizeF  box = m_pressRect.size();
                const QPointF fraction(box.width() > 0 ? (m_pressPos.x() - m_pressRect.left()) / box.width() : 0.5,
                                       box.height() > 0 ? (m_pressPos.y() - m_pressRect.top()) / box.height() : 0.5);
                const QPointer<QTextBrowser> guard(browser);
                const filedrag::Result       result = filedrag::start(m_core, key, this, fraction, browser->devicePixelRatioF(), styleFor(browser).font);
                // Not downloaded yet: the download started; "Drag it again when it's ready".
                if (!result.message.isEmpty() && guard)
                    QToolTip::showText(QCursor::pos(), result.message, guard->viewport(), QRect(), result.error ? qMax(4000, ui::notificationDurationMs()) : 2500);
                return true;
            }
        }
        VideoZone zone = VideoZone::None;
        const Hit hit  = updateHoverAt(browser, me->localPos(), true, &zone);
        // The seek bar's tip shows the time under the pointer: keep it current while it is shown.
        if (zone == VideoZone::Seek && QToolTip::isVisible()) {
            QRect         area;
            const QString tip = toolTipText(browser, hit, me->localPos(), &area);
            if (!tip.isEmpty())
                QToolTip::showText(me->globalPos(), tip, browser->viewport(), area);
        }
        if (!hit.key.isEmpty() && me->buttons() == Qt::NoButton)
            return true; // our cursor, not QTextBrowser's link hover handling
        break;
    }

    case QEvent::Leave: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        if (!m_hoverKey.isEmpty() && m_media) {
            const QString                left   = m_hoverKey;
            const QPointer<QTextBrowser> leftIn = m_hoverBrowser;
            m_hoverKey.clear();
            m_hoverBrowser = nullptr;
            m_hoverZone    = VideoZone::None;
            m_media->hover(QString(), VideoZone::None);
            if (leftIn)
                repaintPointerState(leftIn, left, false);
        }
        if (!m_hoverObject.isEmpty()) { // 2.2 album
            const QString                left   = m_hoverObject;
            const QPointer<QTextBrowser> leftIn = m_hoverObjectIn;
            m_hoverObject.clear();
            m_hoverTile     = -1;
            m_hoverObjectIn = nullptr;
            if (albums::isObjectId(left) && leftIn)
                refreshPreview(leftIn, left, true);
        }
        updateCursor(browser, false, QPoint(-1, -1));
        break;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        auto*     me  = static_cast<QMouseEvent*>(event);
        const Hit hit = previewAt(browser, me->pos());
        if (hit.key.isEmpty()) {
            if (albums::isObjectId(hit.object) && me->button() == Qt::LeftButton)
                return true; // 2.2 album: a gap or an item still on its way; no text selection from there
            break;
        }
        if (me->button() == Qt::RightButton)
            return true; // the context menu event follows
        if (me->button() != Qt::LeftButton)
            break;
        const MediaEntry* e = m_core->entry(hit.key);
        // 2.2 spoiler: a covered video is pressed like a picture (no controls), and the second click of
        // a double click that revealed a spoiler doesn't play or open it as well.
        const bool hidden = m_core->isSpoilerHidden(hit.key);
        const bool video  = e && e->kind == MediaKind::Video && !hidden && hit.tile < 0; // 2.2 album: a tile is no player
        const bool audio  = e && !hidden && hit.tile < 0 && m_media && m_media->mode(hit.key) == InlineMediaController::Mode::Audio; // 2.2 audio
        if (event->type() == QEvent::MouseButtonDblClick && m_reveals && m_reveals->revealedWithin(hit.key, QApplication::doubleClickInterval()))
            return true;
        if (event->type() == QEvent::MouseButtonDblClick && !video && !audio)
            return true; // the first click already opened it
        m_pressedObject        = hit.object; // 2.2 album
        m_pressedTile          = hit.tile;
        m_pressedKey           = hit.key;
        m_pressBrowser         = browser;
        m_pressRect            = hit.rect;
        m_pressPos             = me->pos(); // 2.2 drag-out
        m_lastBrowser          = browser;
        m_pressControlsVisible = audio || (video && m_media && m_media->overlay(hit.key).controlsVisible);
        if (audio) { // 2.2 audio: the card's seek bar is always there; a press on it seeks (and drags)
            const QSize           size  = hit.rect.size().toSize();
            const QPointF         local = me->localPos() - hit.rect.topLeft();
            const PlaybackOverlay o     = m_media->overlay(hit.key);
            const PreviewStyle    style = styleFor(browser);
            if (audioZoneAt(*e, o, style, size, local) == VideoZone::Seek) {
                m_seeking    = true;
                m_lastSeekMs = QDateTime::currentMSecsSinceEpoch();
                m_media->click(hit.key, VideoZone::Seek, audioSeekFractionAt(*e, o, style, size, local));
            }
        } else if (video && m_media) {
            const QSize   size  = hit.rect.size().toSize();
            const QPointF local = me->localPos() - hit.rect.topLeft();
            if (videoZoneAt(size, local, !m_media->overlay(hit.key).playing) == VideoZone::Seek && m_pressControlsVisible) {
                m_seeking    = true;
                m_lastSeekMs = QDateTime::currentMSecsSinceEpoch();
                m_media->click(hit.key, VideoZone::Seek, seekFractionAt(size, local));
            }
        }
        if (albums::isObjectId(hit.object))
            refreshPreview(browser, hit.object, true); // 2.2 album: the pressed tile
        else
            repaintPointerState(browser, hit.key, true); // the press shows before the release acts
        return true;
    }

    case QEvent::MouseButtonRelease: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::RightButton) {
            if (!previewAt(browser, me->pos()).key.isEmpty())
                return true;
            break;
        }
        if (me->button() != Qt::LeftButton || m_pressedKey.isEmpty())
            break;
        const QString pressed       = m_pressedKey;
        const QString pressedObject = m_pressedObject; // 2.2 album
        const int     pressedTile   = m_pressedTile;
        m_pressedKey.clear();
        m_pressedObject.clear();
        m_pressedTile = -1;
        if (m_seeking) {
            m_seeking = false;
            if (m_media)
                m_media->click(pressed, VideoZone::Seek, seekFraction(browser, pressed, m_pressRect.size().toSize(), me->localPos() - m_pressRect.topLeft()));
            return true;
        }
        if (m_pressBrowser == browser) {
            if (albums::isObjectId(pressedObject))
                refreshPreview(browser, pressedObject, true); // 2.2 album
            else
                repaintPointerState(browser, pressed, true);
        }
        const Hit hit = previewAt(browser, me->pos());
        if (hit.key == pressed && hit.object == pressedObject && hit.tile == pressedTile && m_pressBrowser == browser) // 2.2 album: the same tile
            activate(hit, me->localPos(), m_pressControlsVisible);
        return true;
    }

    case QEvent::ContextMenu: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        auto*     ce  = static_cast<QContextMenuEvent*>(event);
        const Hit hit = previewAt(browser, ce->pos());
        if (hit.key.isEmpty())
            break;
        m_lastBrowser = browser;
        showContextMenu(browser, hit.key, ce->globalPos(), hit.tile >= 0 ? hit.object : QString()); // 2.2 album
        return true;
    }

    case QEvent::ToolTip: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        auto*     he  = static_cast<QHelpEvent*>(event);
        const Hit hit = previewAt(browser, he->pos());
        if (hit.key.isEmpty() && hit.tile < 0) { // 2.2 album: an item still on its way has a tip, a gap none
            if (!albums::isObjectId(hit.object))
                break;
            QToolTip::hideText();
            return true;
        }
        QRect         area;
        const QString text = toolTipText(browser, hit, QPointF(he->pos()), &area);
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(he->globalPos(), text, browser->viewport(), area);
        return true;
    }

    // A new chat width re-lays the previews out; a theme switch (palette / style sheet) re-renders
    // them in the new colours.
    case QEvent::Resize:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
        if (QTextBrowser* browser = browserForViewport(watched))
            scheduleRelayout(browser);
        break;

    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* de = static_cast<QDragMoveEvent*>(event);
        if (filedrag::isOwn(de->mimeData())) {
            // 2.2 drag-out: a file dragged out of a chat is not dropped back (not even as a path in the
            // input): the no-drop cursor shows, whatever the settings and modifier keys.
            hideDropOverlay();
            de->setDropAction(Qt::IgnoreAction);
            de->ignore();
            return true;
        }
        if (!s.interceptDragDrop || (de->keyboardModifiers() & Qt::ShiftModifier) || !acceptsDrop(de->mimeData())) {
            hideDropOverlay(); // Shift: TeamSpeak's own drop
            break;
        }
        // Where it would go is checked when the drag enters a widget (or Shift is let go), not on every move.
        // 2.2 compose: what the drop does follows Ctrl live.
        if (event->type() == QEvent::DragEnter || !m_dropOverlay || !m_dropOverlay->isVisible() || dropSendsNow(de) != m_dropNow) {
            if (event->type() == QEvent::DragEnter || !m_dropOverlay || !m_dropOverlay->isVisible())
                m_dropBlock = checkSend(qobject_cast<QWidget*>(watched), &m_dropTarget);
            m_dropNow = dropSendsNow(de);
            showDropOverlay(qobject_cast<QWidget*>(watched), de->mimeData());
        }
        // Kept as the drag target either way; "no drop" when nothing could be sent from here.
        de->setDropAction(m_dropBlock == SendBlock::None ? Qt::CopyAction : Qt::IgnoreAction);
        de->accept();
        return true;
    }

    case QEvent::DragLeave:
        hideDropOverlay();
        break;

    case QEvent::Drop: {
        hideDropOverlay();
        auto* de = static_cast<QDropEvent*>(event);
        if (filedrag::isOwn(de->mimeData())) { // 2.2 drag-out
            de->setDropAction(Qt::IgnoreAction);
            de->ignore();
            return true;
        }
        if (!s.interceptDragDrop || (de->keyboardModifiers() & Qt::ShiftModifier) || !acceptsDrop(de->mimeData()))
            break;
        auto*           widget = qobject_cast<QWidget*>(watched);
        ChatTarget      target;
        const SendBlock block = checkSend(widget, &target);
        if (block != SendBlock::None) {
            warnCantSend(widget, block);
            de->setDropAction(Qt::IgnoreAction);
            de->accept();
            return true;
        }
        // 2.2 compose: the send window, unless the drop sends right away (the setting, or Ctrl). A drop
        // where nothing could be sent opens the window anyway, so it says why before anything fails.
        const QMimeData* mime = de->mimeData();
        bool             now  = dropSendsNow(de);
        if (now && mime->hasUrls()) {
            const qint64 limit = static_cast<qint64>(Settings::instance().forServer(ts3::serverUid(target.sch)).uploadMaxMB) * 1024 * 1024; // 2.2 per-server settings
            bool         any   = false;
            for (const QUrl& url : mime->urls()) {
                const QFileInfo fi(url.toLocalFile());
                any = any || compose::checkFile(fi.exists(), fi.isFile(), fi.size(), limit) == compose::Problem::None;
            }
            now = any;
        }
        if (now) {
            sendMime(mime, target);
        } else {
            QStringList paths;
            for (const QUrl& url : mime->urls())
                paths.append(url.toLocalFile());
            // Copied now: the drag's data is gone once this event returns. Opened after it returns, so the
            // drag source isn't kept waiting.
            const QImage            image = paths.isEmpty() ? qvariant_cast<QImage>(mime->imageData()) : QImage();
            const QPointer<QWidget> source(widget);
            QMetaObject::invokeMethod(this, [this, source, paths, image, target] { openCompose(source.data(), paths, image, target); }, Qt::QueuedConnection);
        }
        de->setDropAction(Qt::CopyAction);
        de->accept();
        return true;
    }

    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!s.interceptPaste || !ke->matches(QKeySequence::Paste))
            break;
        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        if (!mime)
            break;
        const bool files = mime->hasUrls() && acceptsDrop(mime);
        const bool image = !mime->hasUrls() && mime->hasImage() && (!mime->hasText() || mime->text().trimmed().isEmpty());
        if (!files && !image)
            break;
        if (ke->isAutoRepeat())
            return true; // a held Ctrl+V asks once
        auto*           widget = qobject_cast<QWidget*>(watched);
        ChatTarget      target;
        const SendBlock block = checkSend(widget, &target);
        if (block != SendBlock::None) {
            warnCantSend(widget, block); // no prompt for something that would fail anyway
            return true;
        }
        // What is on the clipboard may be old and unseen: nothing is sent before the user saw it.
        // The data is copied now, the clipboard can change while the send window is open.
        QStringList paths;
        if (files) {
            for (const QUrl& url : mime->urls())
                paths.append(url.toLocalFile());
        }
        // 2.2 compose: text already typed in the chat input becomes the caption (one line, up to the
        // caption's length); the input is cleared only after a send, and only if it still holds that text.
        QTextEdit* input = qobject_cast<QTextEdit*>(widget);
        if (!input && widget)
            input = qobject_cast<QTextEdit*>(widget->parentWidget()); // the input's viewport
        const QString typed   = input ? input->toPlainText() : QString();
        const QString trimmed = typed.trimmed();
        const bool    oneLine = !trimmed.contains(QLatin1Char('\n')) && !trimmed.contains(QChar::LineSeparator) && !trimmed.contains(QChar::ParagraphSeparator);
        const bool    prefill = !trimmed.isEmpty() && oneLine && trimmed.size() <= kCaptionMaxChars;
        openCompose(widget, paths, files ? QImage() : qvariant_cast<QImage>(mime->imageData()), target, prefill ? trimmed : QString(),
                    prefill ? input : nullptr, typed);
        return true;
    }

    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// ============================================================================================
// Chat targets
// ============================================================================================

QTabBar* ChatIntegration::chatTabBarFor(QWidget* widget) const
{
    for (QWidget* anc = widget; anc; anc = anc->parentWidget()) {
        QTabBar* fallback = nullptr;
        for (QTabBar* bar : anc->findChildren<QTabBar*>()) {
            if (!bar->isVisible())
                continue;
            if (bar->inherits("ChatTabBar"))
                return bar;
            if (!fallback && bar->parentWidget() && bar->parentWidget()->inherits("ChatTabWidget"))
                fallback = bar;
        }
        if (fallback)
            return fallback;
    }
    return nullptr;
}

// The chat tab is identified by its place, not by guessing from names (a private chat with "Alex"
// must not be mistaken for the channel "Alex's Room"): TeamSpeak's chat tabs are the server tab,
// the channel tab, then private chats; the first two cannot be closed or moved. A private chat
// whose partner cannot be identified (left the server, renamed, ...) is refused: what was meant
// for one person must never end up in the channel or server chat instead.
bool ChatIntegration::resolveTarget(QWidget* widget, ChatTarget* target) const
{
    ChatTarget t;
    t.sch   = ts3::currentConnection();
    t.mode  = TextMessageTarget_CHANNEL;
    *target = t;

    QTabBar*  bar   = widget ? chatTabBarFor(widget) : nullptr;
    const int index = bar ? bar->currentIndex() : -1;
    if (index < 0 || !ts3::isConnected(t.sch))
        return true; // no chat tabs to tell apart (Core reports a missing connection itself)
    target->serverUid = ts3::serverUid(t.sch); // a retry checks it is still the same server

    if (index == 0) {
        target->mode = TextMessageTarget_SERVER;
        return true;
    }
    const TabText tab = cleanTabText(bar->tabText(index));
    if (index == 1 && tabShows(tab, ts3::channelName(t.sch, ts3::ownChannel(t.sch))))
        return true;
    if (const anyID client = clientForTab(t.sch, tab)) {
        target->mode      = TextMessageTarget_CLIENT;
        target->clientId  = client;
        target->clientUid = ts3::clientUid(t.sch, client); // client ids are reused: the message follows the person
        return true;
    }
    return index == 1; // the channel tab (its text may lag behind a channel rename)
}

bool ChatIntegration::resolveCurrentTarget(ChatTarget* target, QWidget** source) const
{
    QWidget* widget = nullptr;
    for (const auto& input : m_inputs) {
        if (input && input->isVisible()) {
            widget = input;
            break;
        }
    }
    if (!widget)
        widget = visibleChatBrowser();
    if (source)
        *source = widget;
    return resolveTarget(widget, target);
}

// Checked before the file picker, the paste prompt and a drop, so the user never picks files that
// can only fail afterwards.
ChatIntegration::SendBlock ChatIntegration::checkSend(QWidget* widget, ChatTarget* target) const
{
    if (!resolveTarget(widget, target))
        return SendBlock::NoRecipient;
    if (!ts3::isConnected(target->sch))
        return SendBlock::NotConnected;
    // Files always go to the own channel's file browser, whichever chat announces them.
    if (ts3::channelHasPassword(target->sch, ts3::ownChannel(target->sch)))
        return SendBlock::Password;
    return SendBlock::None;
}

QString ChatIntegration::blockText(SendBlock block) const
{
    switch (block) {
    case SendBlock::NoRecipient:
        return noRecipientText(); // Core's wording (a retry says the same)
    case SendBlock::NotConnected:
        return notConnectedText(); // Core's wording
    case SendBlock::Password:
        return uploadErrorText(MediaError::Password);
    case SendBlock::None:
        break;
    }
    return {};
}

// What happened, at the place the user acted (a tooltip) and in the chat, where it stays readable.
void ChatIntegration::warnCantSend(QWidget* widget, SendBlock block) const
{
    const QString text = blockText(block);
    if (text.isEmpty())
        return;
    if (QTextBrowser* chat = chatBrowserFor(widget))
        ts3::setChatDark(styleFor(chat).dark);
    ts3::printWarning(ts3::currentConnection(), text);
    if (widget && widget->isVisible())
        QToolTip::showText(widget->mapToGlobal(QPoint(12, widget->height() / 2)), text, widget);
}

QTextBrowser* ChatIntegration::chatBrowserFor(QWidget* widget) const
{
    if (QTextBrowser* browser = browserForViewport(widget))
        return browser;
    // The chat input: the chat shown above it (their nearest common ancestor holds both).
    for (QWidget* anc = widget ? widget->parentWidget() : nullptr; anc; anc = anc->parentWidget()) {
        for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
            QTextBrowser* b = it->browser;
            if (b && b->isVisible() && anc->isAncestorOf(b))
                return b;
        }
    }
    return visibleChatBrowser();
}

// 2.2 compose: Settings::dropOpensSendWindow, the other way round while Ctrl is held.
bool ChatIntegration::dropSendsNow(const QDropEvent* drop) const
{
    const bool ctrl = drop && (drop->keyboardModifiers() & Qt::ControlModifier);
    return Settings::instance().dropOpensSendWindow == ctrl;
}

// Uses m_dropBlock / m_dropTarget, checked when the drag entered widget, and m_dropNow.
void ChatIntegration::showDropOverlay(QWidget* widget, const QMimeData* mime)
{
    QTextBrowser* browser = chatBrowserFor(widget);
    if (!browser || !mime) {
        hideDropOverlay();
        return;
    }
    auto* overlay = static_cast<DropOverlay*>(m_dropOverlay.data());
    if (!overlay) {
        overlay       = new DropOverlay(browser);
        m_dropOverlay = overlay;
    } else if (overlay->parentWidget() != browser) {
        overlay->setParent(browser);
    }
    const PreviewStyle style = styleFor(browser);
    overlay->setFont(style.font);
    overlay->setGeometry(browser->viewport()->geometry());

    QString title;
    QString subtitle;
    QString hint = i18n::t("Hold Shift to drop without sending");
    if (m_dropBlock == SendBlock::None) {
        // 2.2 compose: "now" when the drop sends right away; the hint says what Ctrl does instead.
        const int count = mime->hasUrls() ? mime->urls().size() : 0;
        if (count == 1) {
            // "Drop to send “holiday.jpg”": the name is shortened in the middle, the quotes stay.
            const QString      prefix = m_dropNow ? i18n::t("Drop to send “%1” now") : i18n::t("Drop to send “%1”");
            const QFontMetrics fm(overlay->titleFont());
            const int          room   = qMax(40, static_cast<int>(overlay->textWidth()) - fm.horizontalAdvance(prefix.arg(QString())));
            title = prefix.arg(fm.elidedText(displayFileName(QFileInfo(mime->urls().first().toLocalFile()).fileName()), Qt::ElideMiddle, room));
        } else if (count > 1) {
            title = (m_dropNow ? i18n::t("Drop to send %1 files now") : i18n::t("Drop to send %1 files")).arg(count);
        } else {
            title = m_dropNow ? i18n::t("Drop to send the image now") : i18n::t("Drop to send the image");
        }
        subtitle                = i18n::t("to %1").arg(describeTarget(m_dropTarget));
        const bool    window    = Settings::instance().dropOpensSendWindow;
        const QString shiftHint = i18n::t("Shift: TeamSpeak's own drop");
        if (!m_dropNow)
            hint = i18n::t("Opens the send window") + dot() + (window ? i18n::t("Ctrl: send right away") : i18n::t("Release Ctrl to send right away")) + dot() + shiftHint;
        else
            hint = (window ? i18n::t("Release Ctrl to open the send window") : i18n::t("Hold Ctrl to open the send window first")) + dot() + shiftHint;
    } else {
        title    = i18n::t("Can't send files here");
        subtitle = m_dropBlock == SendBlock::NoRecipient ? i18n::t("Can't tell who this private chat is with (they may have left the server).")
                                                         : blockText(m_dropBlock);
    }
    overlay->setContent(style.dark, m_dropBlock != SendBlock::None, title, subtitle, hint);
    overlay->show();
    overlay->raise();
}

void ChatIntegration::hideDropOverlay()
{
    if (m_dropOverlay && m_dropOverlay->isVisible())
        m_dropOverlay->hide();
}

QString ChatIntegration::describeTarget(const ChatTarget& target) const
{
    switch (target.mode) {
    case TextMessageTarget_SERVER: {
        const QString name = ts3::serverName(target.sch);
        return name.isEmpty() ? i18n::t("everyone on the server")
                              : i18n::t("everyone on the server “%1”").arg(name);
    }
    case TextMessageTarget_CLIENT: {
        const QString name = nicknameOf(target.sch, target.clientId);
        return name.isEmpty() ? i18n::t("this private chat") : i18n::t("%1 (private chat)").arg(name);
    }
    default: {
        const QString name = ts3::isConnected(target.sch) ? ts3::channelName(target.sch, ts3::ownChannel(target.sch)) : QString();
        return name.isEmpty() ? i18n::t("the channel") : i18n::t("the channel “%1”").arg(name);
    }
    }
}

namespace {

// 2.2 compose: a client's unique identity, empty when no such client is on the server.
QString uniqueIdOf(uint64 sch, anyID client)
{
    char* uid = nullptr;
    if (!client || !ts3::funcs.getClientVariableAsString || ts3::funcs.getClientVariableAsString(sch, client, CLIENT_UNIQUE_IDENTIFIER, &uid) != ERROR_ok)
        return {};
    return ts3::takeString(uid);
}

bool sameTarget(const ChatTarget& a, const ChatTarget& b)
{
    return a.sch == b.sch && a.mode == b.mode && a.clientId == b.clientId;
}

} // namespace

// 2.2 compose: the send window replaces 2.1's paste prompt. Non-blocking (no nested event loop inside
// the event filter), owned by us and named "tsmedia..." so neither ChatIntegration's destructor nor
// plugin shutdown can leave it behind.
void ChatIntegration::openCompose(QWidget* source, const QStringList& files, const QImage& image, const ChatTarget& target,
                                  const QString& caption, QTextEdit* input, const QString& inputText)
{
    if (files.isEmpty() && image.isNull())
        return;
    if (m_compose) {
        // It is window-modal: only a hotkey or the menu get here. The same chat gets the new items.
        if (sameTarget(m_compose->target(), target)) {
            m_compose->addFiles(files);
            m_compose->addImage(image);
        }
        m_compose->raise();
        m_compose->activateWindow();
        return;
    }

    // A private chat is with this person for as long as the window is open (ids are reused).
    const bool                      privateChat = target.mode == TextMessageTarget_CLIENT;
    const QString                   partnerUid  = privateChat ? uniqueIdOf(target.sch, target.clientId) : QString();
    const QString                   partnerName = privateChat ? nicknameOf(target.sch, target.clientId) : QString();
    const QPointer<ChatIntegration> self(this);

    ComposeHost host;
    host.describeTarget = [self](const ChatTarget& t) { return self ? self->describeTarget(t) : QString(); };
    host.blocker        = [partnerUid, partnerName](const ChatTarget& t) -> QString {
        if (!ts3::isConnected(t.sch))
            return notConnectedText();
        if (t.mode == TextMessageTarget_CLIENT) {
            if (!t.clientId || partnerUid.isEmpty())
                return i18n::t("Can't tell who this private chat is with (they may have left the server).");
            if (uniqueIdOf(t.sch, t.clientId) != partnerUid)
                return i18n::t("%1 left the server, so nothing can be sent to this private chat.").arg(partnerName);
        }
        // Files always go to the own channel's file browser, whichever chat announces them.
        if (ts3::channelHasPassword(t.sch, ts3::ownChannel(t.sch)))
            return uploadErrorText(MediaError::Password);
        return {};
    };
    // 2.2 per-server settings: the upload size limit, folder and note of the server the chat is on.
    host.uploadLimitMB = [](const ChatTarget& t) { return Settings::instance().forServer(ts3::serverUid(t.sch)).uploadMaxMB; };
    host.linkContext   = [](const ChatTarget& t) {
        const Settings       s = Settings::instance().forServer(ts3::serverUid(t.sch));
        compose::LinkContext context;
        QString              address;
        quint16              port = 0;
        if (ts3::getServerAddress(t.sch, &address, &port)) {
            context.host = address;
            context.port = port;
        }
        const QString uid = ts3::serverUid(t.sch);
        if (!uid.isEmpty())
            context.serverUid = uid;
        context.channelId   = ts3::ownChannel(t.sch);
        context.remoteDir   = Settings::normalizeUploadDirectory(s.uploadDirectory);
        context.previews    = s.generatePreviews;
        context.notice      = s.addRequiredNotice;
        context.downloadUrl = s.pluginDownloadUrl;
        return context;
    };
    host.savePastedImage = [](const QImage& picture) {
        return compose::savePastedImage(picture, ts3::dataDir() + QStringLiteral("/paste"), Settings::instance().convertLargePngToJpeg);
    };
    host.send          = [self](const SendRequest& request) { return self && self->m_core ? self->m_core->send(request) : 0; };
    host.rememberAlbum = [](bool album) {
        Settings& s = Settings::instance();
        if (s.sendAsAlbum != album) {
            s.sendAsAlbum = album;
            s.save();
        }
    };
    host.albumDefault = Settings::instance().sendAsAlbum;
    // 2.4 compress: the Quality combo; 2.2 per-server: with the upload limit of the server the chat is on.
    host.compressOptions = [sch = target.sch] { return Core::compressOptions(SendQuality::Auto, ts3::serverUid(sch)); };
    // 2.2 editor: edited copies under <dataDir>/edit (Core clears it at start-up), the paste rule, and
    // the editor's last choices.
    {
        const Settings& s           = Settings::instance();
        host.editDirectory          = ts3::dataDir() + QStringLiteral("/edit");
        host.convertLargePngToJpeg  = s.convertLargePngToJpeg;
        host.editorPrefs.color      = s.editorColor;
        host.editorPrefs.stroke     = s.editorStroke;
        host.editorPrefs.hideMode   = s.editorHideMode;
        host.rememberEditorPrefs    = [](const imageedit::Prefs& prefs) {
            Settings& settings = Settings::instance();
            if (settings.editorColor == prefs.color && settings.editorStroke == prefs.stroke && settings.editorHideMode == prefs.hideMode)
                return;
            settings.editorColor    = prefs.color;
            settings.editorStroke   = prefs.stroke;
            settings.editorHideMode = prefs.hideMode;
            settings.save();
        };
    }

    QWidget* parent = mainWindow();
    if (!parent && source)
        parent = source->window();
    auto* dialog = new ComposeDialog(std::move(host), target, parent);
    dialog->addFiles(files);
    dialog->addImage(image);
    if (!caption.isEmpty())
        dialog->setPrefilledCaption(caption);
    // The chat input's text went out as the caption: it is cleared once the caption is in the chat
    // (onCaptionSettled), unless it changed meanwhile or the caption was emptied in the window (the text
    // then stays for a message of its own). If the caption never gets posted, the text stays.
    const QPointer<QObject> chatInput(input);
    connect(dialog, &ComposeDialog::sent, this, [this, chatInput, inputText, caption](const QString& sentCaption, int batch) {
        if (chatInput && batch != 0 && !caption.isEmpty() && !sentCaption.isEmpty())
            m_captionClears.insert(batch, {chatInput, inputText});
    });
    m_compose = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// ---- 2.2 voice -------------------------------------------------------------------------------------

bool ChatIntegration::voiceTarget(ChatTarget* target, QString* description, QWidget** anchor)
{
    QWidget* source = nullptr;
    resolveCurrentTarget(target, &source);
    const SendBlock block = checkSend(source, target);
    if (block == SendBlock::Password) {
        // The file goes to the own channel's file browser, which a password channel doesn't allow.
        const QString text = i18n::t("Voice messages can't be sent from password-protected channels. Join another channel, then try again.");
        if (QTextBrowser* chat = chatBrowserFor(source))
            ts3::setChatDark(styleFor(chat).dark);
        ts3::printWarning(ts3::currentConnection(), text);
        if (source && source->isVisible())
            QToolTip::showText(source->mapToGlobal(QPoint(12, source->height() / 2)), text, source);
        return false;
    }
    if (block != SendBlock::None) {
        warnCantSend(source, block);
        return false;
    }
    *description = describeTarget(*target);
    QWidget* input = nullptr; // the recorder sits right above the chat input
    for (const auto& w : m_inputs) {
        if (w && w->isVisible()) {
            input = w;
            break;
        }
    }
    *anchor = input ? input : source;
    return true;
}

void ChatIntegration::pauseAllPlayback()
{
    if (m_media)
        m_media->pauseAll();
    if (m_viewer)
        m_viewer->pausePlayback();
}

// ---- end 2.2 voice ---------------------------------------------------------------------------------

ChatTarget ChatIntegration::currentTarget() const
{
    ChatTarget target;
    if (!resolveCurrentTarget(&target, nullptr)) {
        // A private chat whose partner is unknown: a target nothing is delivered to, never the channel.
        target.mode     = TextMessageTarget_CLIENT;
        target.clientId = 0;
    }
    return target;
}

QTextBrowser* ChatIntegration::visibleChatBrowser() const
{
    QTextBrowser* fallback = nullptr;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        QTextBrowser* b = it->browser;
        if (!b || !b->isVisible())
            continue;
        if (b->window()->isActiveWindow())
            return b;
        if (!fallback)
            fallback = b;
    }
    return fallback;
}

void ChatIntegration::pickAndSendFiles()
{
    static QString lastDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    ChatTarget     target;
    QWidget*       source = nullptr;
    resolveCurrentTarget(&target, &source); // finds the chat input (or chat) the command came from
    // 2.2 compose: while the send window is open (window-modal: the hotkey or the menu), it is raised,
    // and for the same chat its own picker adds the files.
    if (m_compose) {
        m_compose->raise();
        m_compose->activateWindow();
        if (sameTarget(m_compose->target(), target))
            m_compose->addFromPicker();
        return;
    }
    // Checked before the picker opens: choosing files that can only fail afterwards wastes the effort.
    const SendBlock block = checkSend(source, &target);
    if (block != SendBlock::None) {
        warnCantSend(source, block);
        return;
    }
    // The title names the destination, like the send window: "Send files to the channel “Lobby”".
    const QPointer<ChatIntegration> guard(this); // the picker runs its own event loop
    const QPointer<QWidget>         from(source);
    const QStringList               files = QFileDialog::getOpenFileNames(
        mainWindow(), i18n::t("Send files to %1").arg(describeTarget(target)), lastDir,
        i18n::t("All files (*.*);;Images (*.png *.jpg *.jpeg *.jfif *.gif *.webp *.bmp);;Videos (*.mp4 *.webm *.mkv *.mov *.avi *.wmv *.m4v *.3gp *.mts *.m2ts *.mpg)"));
    if (!guard || files.isEmpty())
        return;
    lastDir = QFileInfo(files.first()).absolutePath();
    openCompose(from.data(), files, QImage(), target); // 2.2 compose: caption, spoilers, what can't be sent
}

// 2.2 compose: the caption that came from the chat input is in the chat now (or never will be).
void ChatIntegration::onCaptionSettled(int batch, bool posted)
{
    const CaptionClear clear = m_captionClears.take(batch);
    auto*              input = qobject_cast<QTextEdit*>(clear.input.data());
    if (posted && input && input->toPlainText() == clear.text)
        input->clear();
}

void ChatIntegration::onUploadChanged(int id)
{
    QTextBrowser* host = visibleChatBrowser();
    if (!host)
        return;
    if (!m_toast)
        m_toast = new UploadToast(m_core, host);
    else if (m_toast->parentWidget() != host)
        m_toast->setHost(host);
    const bool dark = styleFor(host).dark; // TeamSpeak's light or dark theme, like the previews
    m_toast->setDark(dark);
    ts3::setChatDark(dark);
    m_toast->updateJob(id);
}

// ============================================================================================
// Diagnostics
// ============================================================================================

// Test builds only (-DTSMEDIA_TESTHOOKS=ON): numbered snapshots of a chat after previews were
// inserted / refreshed / animated; at most one per 1.5 s per chat, the last 30 are kept.
void ChatIntegration::requestSnapshot(QTextBrowser* browser, const QString& reason)
{
#ifdef TSMEDIA_TESTHOOKS
    View* view = viewFor(browser);
    if (!view)
        return;
    view->snapshotReason = reason;
    if (view->snapshotPending)
        return;
    view->snapshotPending = true;
    const qint64 now      = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait     = qMax<qint64>(400, view->lastSnapshotMs + 1500 - now);
    QPointer<QTextBrowser> guard(browser);
    singleShotOwned(static_cast<int>(wait), this, [this, guard] {
        View* v = guard ? viewFor(guard.data()) : nullptr;
        if (!v)
            return;
        v->snapshotPending = false;
        v->lastSnapshotMs  = QDateTime::currentMSecsSinceEpoch();
        const int     number = ++v->snapshotCount;
        const QString dir    = ts3::dataDir() + QStringLiteral("/debug");
        const QString id     = QString::number(reinterpret_cast<quintptr>(guard.data()), 16);
        auto          name   = [&id](int n) { return QStringLiteral("chat_%1_%2.png").arg(id).arg(n, 2, 10, QLatin1Char('0')); };
        QDir().mkpath(dir);
        guard->grab().save(dir + QLatin1Char('/') + name(number));
        if (number > 30)
            QFile::remove(dir + QLatin1Char('/') + name(number - 30));
        ts3::log(QStringLiteral("[test] snapshot %1 (%2)").arg(name(number), v->snapshotReason));
    });
#else
    Q_UNUSED(browser);
    Q_UNUSED(reason);
#endif
}

QString ChatIntegration::dumpWidgetTree() const
{
    QString out;
    std::function<void(const QWidget*, int)> dump = [&](const QWidget* w, int depth) {
        out += QString(depth * 2, QLatin1Char(' ')) + QString::fromLatin1(w->metaObject()->className());
        if (!w->objectName().isEmpty())
            out += QStringLiteral(" #") + w->objectName();
        out += QStringLiteral(" [%1,%2 %3x%4]%5").arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height()).arg(w->isVisible() ? QString() : QStringLiteral(" hidden"));
        if (auto* bar = qobject_cast<const QTabBar*>(w)) {
            QStringList tabs;
            for (int i = 0; i < bar->count(); ++i)
                tabs << bar->tabText(i);
            out += QStringLiteral(" tabs=") + tabs.join(QStringLiteral(" | ")) + QStringLiteral(" current=") + QString::number(bar->currentIndex());
        }
        if (auto* tb = qobject_cast<const QTextBrowser*>(w)) {
            int links = 0, previews = 0;
            for (QTextBlock block = tb->document()->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat cf = it.fragment().charFormat();
                    if (isTs3FileAnchor(cf))
                        ++links;
                    if (!previewKeyOf(cf).isEmpty())
                        ++previews;
                }
            }
            out += QStringLiteral(" blocks=%1 ts3fileLinks=%2 previews=%3 tracked=%4").arg(tb->document()->blockCount()).arg(links).arg(previews).arg(m_views.contains(const_cast<QTextBrowser*>(tb)) ? 1 : 0);
        }
        out += QLatin1Char('\n');
        for (QObject* child : w->children()) {
            if (auto* cw = qobject_cast<QWidget*>(child))
                dump(cw, depth + 1);
        }
    };
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (top->isVisible())
            dump(top, 0);
    }
    return out;
}

// ============================================================================================
// 2.2 album: the document helpers above, for chatalbums.cpp
// ============================================================================================

QString ChatIntegration::resourceName(const QString& id)
{
    return kScheme + id;
}

QString ChatIntegration::objectIdOf(const QTextCharFormat& format)
{
    return previewKeyOf(format);
}

bool ChatIntegration::isOwnSeparator(QTextDocument* document, int position)
{
    return isOurSeparator(document, position);
}

bool ChatIntegration::isFileAnchor(const QTextCharFormat& format)
{
    return isTs3FileAnchor(format);
}

bool ChatIntegration::atBottom(QTextBrowser* browser)
{
    return isAtBottom(browser);
}
