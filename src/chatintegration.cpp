#include "chatintegration.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
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

#include "i18n.h"
#include "inlinemedia.h"
#include "mediaviewer.h"
#include "settings.h"
#include "uiutil.h"
#include "uploadtoast.h"

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

// A paste prompt lists at most this many file names.
constexpr int kPromptMaxNames = 6;

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
    // Inline players are shut down here, synchronously, while Core still exists.
    delete m_media;
    m_media = nullptr;
    if (m_pasteConfirm)
        delete m_pasteConfirm.data();

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

    // Created after our own entryChanged connection, so previews are re-laid out (and frame sizes
    // updated) before the controller reacts to the same change.
    m_media = new InlineMediaController(m_core, this);
    connect(m_media, &InlineMediaController::frameChanged, this, &ChatIntegration::onFrameChanged);
    connect(m_media, &InlineMediaController::openRequested, this, &ChatIntegration::openViewer, Qt::QueuedConnection);
    connect(m_media, &InlineMediaController::playbackStarted, this, [this] {
        if (m_viewer)
            m_viewer->pausePlayback();
    });

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
    connect(browser->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { scheduleVisibilityUpdate(); });

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
    QTimer::singleShot(60, this, [this, guard] {
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

            if (!hasPreview && previewsOn && !raw)
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
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return {};

    PreviewStyle style   = styleFor(browser);
    const bool   hasDims = e->link.width > 0 && e->link.height > 0;
    const bool   media   = isMediaKind(e->kind);
    // Pointer feedback: hovered while the mouse is over it, pressed while the left button that went
    // down on it is held there (a seek drag is not a press of the picture).
    style.hovered    = key == m_hoverKey;
    style.pressed    = style.hovered && key == m_pressedKey && !m_seeking;
    style.revealOnly = !media && e->state == MediaState::Ready && m_core->isUnsafeToOpen(key);
    const QSize stillPixels = stillPixelsFor(*e, style);
    QSize       size;
    QImage      img;
    bool        fromStill = false;

    const InlineMediaController::Mode mode = m_media ? m_media->mode(key) : InlineMediaController::Mode::Still;
    if (mode == InlineMediaController::Mode::Video) {
        const QImage     frame  = m_media->frame(key);
        const MediaStill poster = frame.isNull() ? m_core->still(key, stillPixels) : MediaStill();
        PlaybackOverlay  o      = m_media->overlay(key);
        if (style.pressed) // hidden controls can't be aimed at: the press is on the picture
            o.pressed = m_pressControlsVisible ? o.hover : VideoZone::Body;
        img = renderVideo(*e, frame, poster, o, style, &size);
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
        if (bottom)
            browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
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
    QTimer::singleShot(200, this, [this, guard] {
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
        const QRectF r = previewRect(browser, p, contentSizeOf(cf.toImageFormat()));
        if (r.contains(viewportPos))
            return {key, r};
    }
    return {};
}

void ChatIntegration::scheduleVisibilityUpdate()
{
    if (m_visibilityQueued)
        return;
    m_visibilityQueued = true;
    QTimer::singleShot(30, this, [this] { updateVisibleKeys(); });
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
            if (previewRect(b, pp.position, QSizeF(view.formatSizes.value(pp.key))).intersects(viewport))
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
    if (mode == InlineMediaController::Mode::Still || (includeVideos && mode == InlineMediaController::Mode::Video))
        refreshPreview(browser, key, true);
}

// The tooltip of the preview under viewportPos, as rich text (empty: none). Video controls get
// their name ("Pause", "Mute", the time under the pointer on the seek bar), and area is narrowed to
// that control so the tip goes away when the pointer leaves it. Everything else gets the file's
// name and size, plus the full status where the preview shortens it or explains a failure.
// Built from i18n::t / arg() only: QToolTip's label outlives the plugin DLL.
QString ChatIntegration::toolTipText(QTextBrowser* browser, const Hit& hit, const QPointF& viewportPos, QRect* area) const
{
    const MediaEntry* e = m_core->entry(hit.key);
    if (!e)
        return {};
    *area = hit.rect.toAlignedRect();

    QString detail;
    if (e->kind == MediaKind::Video && m_media) {
        const PlaybackOverlay o = m_media->overlay(hit.key);
        if (o.busy) {
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

    if (e->state == MediaState::Failed) {
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

    // The name comes from someone else's chat link: shown as plain text (escaped, since a tool tip
    // would interpret markup), without bidi/control characters.
    QString name = displayNameFor(e->link);
    if (e->link.size)
        name += QStringLiteral(" · ") + formatSize(e->link.size);
    QString text = QStringLiteral("<div style='white-space:pre'>%1</div>").arg(name.toHtmlEscaped());
    if (!detail.isEmpty())
        text += QStringLiteral("<div>%1</div>").arg(detail.toHtmlEscaped());
    return text;
}

void ChatIntegration::activate(const Hit& hit, const QPointF& viewportPos, bool controlsVisible)
{
    const MediaEntry* e = m_core->entry(hit.key);
    if (!e)
        return;
    const QString key = hit.key;

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
        return v->positionsByKey.contains(key);
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
            if (seen.contains(pp.key))
                continue;
            seen.insert(pp.key);
            const MediaEntry* e = m_core->entry(pp.key);
            if (e && isMediaKind(e->kind))
                keys.append(pp.key);
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
    m_media->pauseAll();
    // The viewer starts with the chat's session mute, and the inline players follow what the user
    // changes there. Only one thing plays at a time: playback starting in the viewer pauses the inline
    // players, and an inline video starting pauses the viewer (see start()).
    if (MediaViewer* viewer = MediaViewer::open(m_core, keys, index, m_media->isMuted())) {
        m_viewer = viewer;
        connect(viewer, &MediaViewer::mutedChanged, m_media, &InlineMediaController::setMuted, Qt::UniqueConnection);
        connect(viewer, &MediaViewer::playbackStarted, m_media, &InlineMediaController::pauseAll, Qt::UniqueConnection);
    }
}

void ChatIntegration::copyImage(const QString& key)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e || e->state != MediaState::Ready || !isPreviewableImage(e->kind))
        return;
    QImageReader reader(e->localPath);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaxCopyPixels)
        return;
    const QImage image = reader.read();
    if (!image.isNull())
        QGuiApplication::clipboard()->setImage(image);
}

void ChatIntegration::showContextMenu(QTextBrowser* browser, const QString& key, const QPoint& globalPos)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e)
        return;
    const bool ready = e->state == MediaState::Ready;
    const bool media = isMediaKind(e->kind);

    auto* menu = new QMenu(browser);
    menu->setObjectName(QStringLiteral("tsmediaPreviewMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);

    if (e->kind == MediaKind::Video && m_media) {
        const PlaybackOverlay o      = m_media->overlay(key);
        const bool            active = o.playing || o.ended || o.positionMs > 0;
        menu->addAction(o.playing ? i18n::t("Pause") : i18n::t("Play"), this, [this, key] {
            if (m_media)
                m_media->click(key, VideoZone::PlayPause, 0.0);
        });
        if (active) {
            menu->addAction(o.muted ? i18n::t("Unmute") : i18n::t("Mute"), this, [this, key] {
                if (!m_media)
                    return;
                // The player may have been closed meanwhile; Mute must never start playback.
                const PlaybackOverlay now = m_media->overlay(key);
                if (now.playing || now.ended || now.positionMs > 0)
                    m_media->click(key, VideoZone::Mute, 0.0);
            });
        }
        menu->addSeparator();
    }

    menu->addAction(i18n::t("Open"), this, [this, key] { openKey(key); });
    if (media && ready)
        menu->addAction(i18n::t("Open with default app"), this, [this, key] { m_core->openExternally(key); });
    if (ready) {
        QPointer<QTextBrowser> guard(browser);
        menu->addAction(i18n::t("Save as…"), this, [this, key, guard] { m_core->saveAs(key, guard ? guard->window() : mainWindow()); });
    }
    if (ready && isPreviewableImage(e->kind))
        menu->addAction(i18n::t("Copy image"), this, [this, key] { copyImage(key); });
    menu->addAction(i18n::t("Copy link"), this, [this, key] {
        if (const MediaEntry* entry = m_core->entry(key))
            QGuiApplication::clipboard()->setText(entry->link.toUrl());
    });
    if (ready)
        menu->addAction(i18n::t("Show in folder"), this, [this, key] { m_core->revealInFolder(key); });

    if (e->state == MediaState::Idle) {
        menu->addSeparator();
        menu->addAction(i18n::t("Download"), this, [this, key] { m_core->download(key, false); });
    } else if (e->state == MediaState::Failed && e->error != MediaError::NotFound) {
        menu->addSeparator();
        menu->addAction(i18n::t("Retry download"), this, [this, key] { m_core->retry(key); });
    }

    menu->popup(globalPos);
}

bool ChatIntegration::acceptsDrop(const QMimeData* mime) const
{
    if (!mime)
        return false;
    for (const QString& format : mime->formats()) {
        if (format.contains(QLatin1String("ts3"), Qt::CaseInsensitive))
            return false; // drags from TeamSpeak's own file browser keep their native behaviour
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

bool ChatIntegration::eventFilter(QObject* watched, QEvent* event)
{
    const Settings& s = Settings::instance();

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
                    const double fraction  = seekFractionAt(m_pressRect.size().toSize(), me->localPos() - m_pressRect.topLeft());
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
        const Hit         hit   = previewAt(browser, me->pos());
        const MediaEntry* entry = hit.key.isEmpty() ? nullptr : m_core->entry(hit.key);
        VideoZone         zone  = VideoZone::None;
        if (!hit.key.isEmpty()) {
            zone = m_media->mode(hit.key) == InlineMediaController::Mode::Video
                       ? videoZoneAt(hit.rect.size().toSize(), me->localPos() - hit.rect.topLeft(), !m_media->overlay(hit.key).playing)
                       : VideoZone::Body;
        }
        if (hit.key != m_hoverKey) {
            const QString left = m_hoverKey;
            m_hoverKey         = hit.key;
            repaintPointerState(browser, left, false);
            repaintPointerState(browser, hit.key, false);
        }
        m_media->hover(hit.key, zone);
        updateCursor(browser, !hit.key.isEmpty(), me->pos(), !entry || isPreviewActionable(*entry));
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
            const QString left = m_hoverKey;
            m_hoverKey.clear();
            m_media->hover(QString(), VideoZone::None);
            repaintPointerState(browser, left, false);
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
        if (hit.key.isEmpty())
            break;
        if (me->button() == Qt::RightButton)
            return true; // the context menu event follows
        if (me->button() != Qt::LeftButton)
            break;
        const MediaEntry* e     = m_core->entry(hit.key);
        const bool        video = e && e->kind == MediaKind::Video;
        if (event->type() == QEvent::MouseButtonDblClick && !video)
            return true; // the first click already opened it
        m_pressedKey           = hit.key;
        m_pressBrowser         = browser;
        m_pressRect            = hit.rect;
        m_lastBrowser          = browser;
        m_pressControlsVisible = video && m_media && m_media->overlay(hit.key).controlsVisible;
        if (video && m_media) {
            const QSize   size  = hit.rect.size().toSize();
            const QPointF local = me->localPos() - hit.rect.topLeft();
            if (videoZoneAt(size, local, !m_media->overlay(hit.key).playing) == VideoZone::Seek && m_pressControlsVisible) {
                m_seeking    = true;
                m_lastSeekMs = QDateTime::currentMSecsSinceEpoch();
                m_media->click(hit.key, VideoZone::Seek, seekFractionAt(size, local));
            }
        }
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
        const QString pressed = m_pressedKey;
        m_pressedKey.clear();
        if (m_seeking) {
            m_seeking = false;
            if (m_media)
                m_media->click(pressed, VideoZone::Seek, seekFractionAt(m_pressRect.size().toSize(), me->localPos() - m_pressRect.topLeft()));
            return true;
        }
        if (m_pressBrowser == browser)
            repaintPointerState(browser, pressed, true);
        const Hit hit = previewAt(browser, me->pos());
        if (hit.key == pressed && m_pressBrowser == browser)
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
        showContextMenu(browser, hit.key, ce->globalPos());
        return true;
    }

    case QEvent::ToolTip: {
        QTextBrowser* browser = browserForViewport(watched);
        if (!browser)
            break;
        auto*     he  = static_cast<QHelpEvent*>(event);
        const Hit hit = previewAt(browser, he->pos());
        if (hit.key.isEmpty())
            break;
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
        if (!s.interceptDragDrop || (de->keyboardModifiers() & Qt::ShiftModifier) || !acceptsDrop(de->mimeData()))
            break;
        de->setDropAction(Qt::CopyAction);
        de->accept();
        return true;
    }

    case QEvent::Drop: {
        auto* de = static_cast<QDropEvent*>(event);
        if (!s.interceptDragDrop || (de->keyboardModifiers() & Qt::ShiftModifier) || !acceptsDrop(de->mimeData()))
            break;
        auto*      widget = qobject_cast<QWidget*>(watched);
        ChatTarget target;
        if (resolveTarget(widget, &target))
            sendMime(de->mimeData(), target);
        else
            warnNoRecipient(widget);
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
        auto*      widget = qobject_cast<QWidget*>(watched);
        ChatTarget target;
        if (!resolveTarget(widget, &target)) {
            warnNoRecipient(widget);
            return true;
        }
        // What is on the clipboard may be old and unseen: nothing is sent before the user saw it.
        // The data is copied now, the clipboard can change while the prompt is open.
        QStringList paths;
        if (files) {
            for (const QUrl& url : mime->urls())
                paths.append(url.toLocalFile());
        }
        confirmPaste(widget, paths, files ? QImage() : qvariant_cast<QImage>(mime->imageData()), target);
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

    if (index == 0) {
        target->mode = TextMessageTarget_SERVER;
        return true;
    }
    const TabText tab = cleanTabText(bar->tabText(index));
    if (index == 1 && tabShows(tab, ts3::channelName(t.sch, ts3::ownChannel(t.sch))))
        return true;
    if (const anyID client = clientForTab(t.sch, tab)) {
        target->mode     = TextMessageTarget_CLIENT;
        target->clientId = client;
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

void ChatIntegration::warnNoRecipient(QWidget* widget) const
{
    const QString text = i18n::t("Can't tell who this private chat is with (they may have left the server). Nothing was sent.");
    ts3::printWarning(ts3::currentConnection(), text);
    if (widget && widget->isVisible())
        QToolTip::showText(widget->mapToGlobal(QPoint(12, widget->height() / 2)), text, widget);
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
        return i18n::t("%1 (private chat)").arg(name.isEmpty() ? QStringLiteral("?") : name);
    }
    default: {
        const QString name = ts3::isConnected(target.sch) ? ts3::channelName(target.sch, ts3::ownChannel(target.sch)) : QString();
        return name.isEmpty() ? i18n::t("the channel") : i18n::t("the channel “%1”").arg(name);
    }
    }
}

// Non-blocking (no nested event loop inside the event filter), owned by us and named "tsmedia..."
// so neither ChatIntegration's destructor nor plugin shutdown can leave it behind. Every text is a
// plain-text label: file names come from the user's disk and must be shown literally.
void ChatIntegration::confirmPaste(QWidget* input, const QStringList& files, const QImage& image, const ChatTarget& target)
{
    if (files.isEmpty() && image.isNull())
        return;
    if (m_pasteConfirm)
        delete m_pasteConfirm.data(); // the newest paste replaces an unanswered one

    QWidget* parent = input ? input->window() : mainWindow();
    auto*    box    = new QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    box->setObjectName(QStringLiteral("tsmediaPasteConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setWindowTitle(i18n::t("Send to chat"));

    const QString where = describeTarget(target);
    QString       question;
    QStringList   details;
    QPixmap       picture;
    if (!files.isEmpty()) {
        quint64 total = 0;
        for (const QString& path : files) {
            const QFileInfo info(path);
            total += static_cast<quint64>(qMax<qint64>(0, info.size()));
            if (details.size() < kPromptMaxNames)
                details.append(displayFileName(info.fileName()));
        }
        if (files.size() == 1) {
            question = i18n::t("Send “%1” to %2?").arg(details.first(), where);
            details.clear();
        } else {
            question = i18n::t("Send %1 files to %2?").arg(files.size()).arg(where);
            if (files.size() > kPromptMaxNames)
                details.append(i18n::t("… and %1 more").arg(files.size() - kPromptMaxNames));
        }
        details.append(formatSize(total));
        picture = box->style()->standardIcon(QStyle::SP_MessageBoxQuestion, nullptr, box).pixmap(32, 32);
    } else {
        question = i18n::t("Send the pasted image to %1?").arg(where);
        details.append(QStringLiteral("%1 × %2").arg(image.width()).arg(image.height()));
        const qreal dpr = box->devicePixelRatioF();
        picture         = QPixmap::fromImage(image.scaled(QSize(200, 150) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        picture.setDevicePixelRatio(dpr);
    }

    auto* pictureLabel = new QLabel(box);
    pictureLabel->setPixmap(picture);
    pictureLabel->setAlignment(Qt::AlignTop);
    auto* questionLabel = new QLabel(question, box);
    questionLabel->setTextFormat(Qt::PlainText);
    questionLabel->setWordWrap(true);
    QFont bold = questionLabel->font();
    bold.setBold(true);
    questionLabel->setFont(bold);
    auto* detailsLabel = new QLabel(details.join(QLatin1Char('\n')), box);
    detailsLabel->setTextFormat(Qt::PlainText);
    detailsLabel->setWordWrap(true);

    auto*        buttons = new QDialogButtonBox(box);
    QPushButton* send    = buttons->addButton(i18n::t("Send"), QDialogButtonBox::AcceptRole);
    buttons->addButton(i18n::t("Cancel"), QDialogButtonBox::RejectRole);
    send->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, box, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, box, &QDialog::reject);

    auto* texts = new QVBoxLayout;
    texts->addWidget(questionLabel);
    texts->addWidget(detailsLabel);
    texts->addStretch(1);
    auto* row = new QHBoxLayout;
    row->addWidget(pictureLabel);
    row->addLayout(texts, 1);
    auto* layout = new QVBoxLayout(box);
    layout->addLayout(row);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    questionLabel->setMinimumWidth(320);

    connect(box, &QDialog::accepted, this, [this, files, image, target] {
        if (!files.isEmpty())
            m_core->uploadFiles(files, target);
        else
            m_core->uploadImage(image, target);
    });
    m_pasteConfirm = box;
    box->show();
    box->raise();
    box->activateWindow();
    send->setFocus();
}

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
    if (!resolveCurrentTarget(&target, &source)) {
        warnNoRecipient(source);
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        mainWindow(), i18n::t("Send files to chat"), lastDir,
        i18n::t("All files (*.*);;Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;Videos (*.mp4 *.webm *.mkv *.mov)"));
    if (files.isEmpty())
        return;
    lastDir = QFileInfo(files.first()).absolutePath();
    m_core->uploadFiles(files, target);
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
    QTimer::singleShot(static_cast<int>(wait), this, [this, guard] {
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
