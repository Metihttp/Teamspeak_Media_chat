#include "chatlinkpreviews.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHelpEvent>
#include <QJsonObject>
#include <QImageReader>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <utility>

#include "chatintegration.h"
#include "chatlayout.h"
#include "chatscale.h" // 2.5 zoom
#include "chatreplies.h"
#include "chatscroll.h"
#include "chattextreactions.h"
#include "i18n.h"
#include "layoutformat.h"
#include "floodgovernor.h" // 2.5: the LPX pictures wait for the flood model's reserve
#include "linkpreview/autoload.h"
#include "linkpreview/nettrace.h" // live-test builds: the driver's links.net
#include "linkpreview/fetcher.h"
#include "linkpreview/hostgate.h"
#include "linkpreview/lphub.h" // usePluginLog, linksCacheDir, Hub
#include "linkpreview/netpolicy.h" // expiredSignedLink, registrableDomain
#include "linkpreview/previewstore.h"
#include "linkpreview/providers.h"
#include "linkpreview/senderpipeline.h"
#include "linkpreview/transport.h"
#include "mediaprobe.h"
#include "peers.h"
#include "safeimage.h"
#include "settings.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "ts3api.h"
#include "inlinemedia.h"            // 2.5 YouTube player: TS Media's session mute
#include "ytplayer/ytbackend.h"     // ... the helper (embedded), the runtime check
#include "ytplayer/ytpage.h"
#include "ytplayer/ytplayer.h"

namespace {

constexpr int kGapTop     = 4; // transparent room above and below a card in its picture (du; 2.5 zoom: x s)
constexpr int kGapBottom  = 4;
constexpr int kMaxThumbs  = 48;
constexpr int kSpinMs     = 50;
constexpr int kAutoDelayMs   = 100;  // 2.4 auto-load: the queue runs this long after a paint saw candidates
constexpr int kWaitTickMs    = 1000; // ... and the waits are looked at again this often while one is on screen
constexpr int kMaxHeld       = 1024; // ... slots held back at most (more: all forgotten, looked at again)
constexpr int kMaxOwnApplied = 64;
constexpr int kMaxLpxPictures = 64;   // 2.5: LPX files waiting to be asked of Core (newest first)
constexpr int kOnScreenMargin = 400;  // a slot within this of the view is "on screen" (lazy loads, LPX pictures)
constexpr qint64 kStillCoolingMs = 60000; // a "Site busy" strip over this is still (no ring)

qint64 wallMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 wallSec()
{
    return QDateTime::currentSecsSinceEpoch();
}

QString waitName(linkstate::Wait wait)
{
    switch (wait) {
    case linkstate::Wait::Own:
        return QString::fromLatin1("own");
    case linkstate::Wait::Sender:
        return QString::fromLatin1("sender");
    case linkstate::Wait::Checking:
        return QString::fromLatin1("checking");
    case linkstate::Wait::Auto:
        return QString::fromLatin1("auto");
    case linkstate::Wait::Host:
        return QString::fromLatin1("host");
    case linkstate::Wait::None:
        break;
    }
    return QString::fromLatin1("-");
}

// What the slot waits for, as linkstate says it.
linkstate::Wait toLinkstate(linkpreview::autoload::Wait wait)
{
    switch (wait) {
    case linkpreview::autoload::Wait::Own:
        return linkstate::Wait::Own;
    case linkpreview::autoload::Wait::Sender:
        return linkstate::Wait::Sender;
    case linkpreview::autoload::Wait::Checking:
        return linkstate::Wait::Checking;
    case linkpreview::autoload::Wait::None:
        break;
    }
    return linkstate::Wait::None;
}

QPixmap blankPicture()
{
    QPixmap pm(1, 1);
    pm.fill(Qt::transparent);
    return pm;
}

QString lookName(linkcard::Look look)
{
    switch (look) {
    case linkcard::Look::Click:
        return QString::fromLatin1("click");
    case linkcard::Look::Loading:
        return QString::fromLatin1("loading");
    case linkcard::Look::Error:
        return QString::fromLatin1("error");
    case linkcard::Look::TooLarge:
        return QString::fromLatin1("toolarge");
    case linkcard::Look::YouTube:
        return QString::fromLatin1("youtube");
    case linkcard::Look::Poster:
        return QString::fromLatin1("poster");
    case linkcard::Look::Hidden:
        return QString::fromLatin1("hidden");
    case linkcard::Look::Embed:
        return QString::fromLatin1("embed");
    case linkcard::Look::Absent:
        return QString::fromLatin1("absent");
    }
    return {};
}

QString kindText(linkpreview::Kind kind)
{
    return QString::fromLatin1(linkpreview::kindName(kind)); // net-core's names ("youtube", "image", ...)
}

bool spinning(const linkcard::Card& card)
{
    // 2.5: "Site busy" for more than a minute is still (nothing spins for minutes).
    const bool still = card.waiting == linkcard::Waiting::Host && card.retryInMs > kStillCoolingMs;
    return (card.look == linkcard::Look::Loading && card.progress < 0 && !still) || (card.thumbnail.isNull() && card.thumbnailLoading)
           || (card.look == linkcard::Look::Embed && card.picture.isNull() && card.pictureLoading)
           || (card.look == linkcard::Look::YouTube && card.player == linkcard::Player::Starting); // 2.5: the player starts
}

// 2.5: the capability a sender announces for this link's class (l, lx, lw): what the waits ask about.
const char* capOf(const linkpreview::Target& target)
{
    const char* cap = linkpreview::autoload::capFor(target.cls);
    return cap ? cap : "l";
}

} // namespace

// ---- the module ---------------------------------------------------------------------------------------------

ChatLinkPreviews::ChatLinkPreviews(ChatIntegration* chat, Core* core)
    : QObject(chat)
    , m_chat(chat)
    , m_core(core)
{
    m_tag     = QString::number(QRandomGenerator::global()->generate() & 0xffffff, 16) + QLatin1Char('-');
    // The chat's clicks and automatic loads (the sender pipeline has its own, linkpreview::Hub). Its log lines say
    // "chat"; which of the two a load was is logged when it starts ("(clicked)" / "(automatic)"). 2.5: it shares the
    // plugin's one HostGate with the sender's Fetcher (spacing, cooldowns and walls per site).
    linkpreview::Fetcher::Options fetchOptions;
    fetchOptions.gate = linkpreview::sharedGate();
#ifdef TSMEDIA_TESTHOOKS
    m_fetcher = new linkpreview::Fetcher(linkpreview::nettrace::traced(linkpreview::http::winHttpTransport(), "chat"), fetchOptions, this);
#else
    m_fetcher         = new linkpreview::Fetcher(linkpreview::http::winHttpTransport(), fetchOptions, this);
#endif
    linkpreview::usePluginLog(m_fetcher, "chat");
    m_loader  = [this](const linkpreview::Target& target, const linkpreview::Caps& caps, const QString& cacheDir) {
        if (m_fetcher)
            m_fetcher->load(target, caps, cacheDir);
    };
    // The fetcher signals on our thread. Its results come a turn of the event loop later (never inside its own call),
    // through a posted functor, not a queued connection: a queued linkpreview::Result would need Qt's metatype
    // registry, which keeps this DLL's copy and delete functions after the plugin is unloaded, and TeamSpeak's
    // Options > Addons > Reload can load it again at another address (live pass C: a crash in QMetaType::create).
    connect(m_fetcher, &linkpreview::Fetcher::progress, this, &ChatLinkPreviews::onProgress, Qt::QueuedConnection); // built-in types only
    connect(
        m_fetcher, &linkpreview::Fetcher::finished, this,
        [this](const QString& key, const linkpreview::Result& result) {
            QMetaObject::invokeMethod(this, [this, key, result] { onFetched(key, result); }, Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    connect(m_core, &Core::entryChanged, this, &ChatLinkPreviews::onEntryChanged, Qt::QueuedConnection);
    // Clear cache: the website files are gone (Core deleted cache/links with the rest), so are the senders'
    // announcements (linkpreview::Hub clears link-previews.json): every card starts over. A link whose sender's
    // preview went with them never loads from its website by itself (PreviewStore::clearedEarlier: a click card).
    connect(m_core, &Core::cacheCleared, this, [this] {
        m_thumbs.clear();
        m_thumbOrder.clear();
        m_ensured.clear();
        m_cacheMiss.clear(); // 2.4 auto-load: nothing is in cache/links any more
        forgetMissingFiles();
        scheduleRefresh();
    });
    m_pool.setMaxThreadCount(1);

    m_refreshTimer = new QTimer(this); // owned: stops with us
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(30);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        const QList<QTextBrowser*> browsers = m_views.keys();
        for (QTextBrowser* b : browsers)
            reconcile(b);
        stopUnneededFetches();
        if (m_yt)
            m_yt->relayout(); // 2.5: a player whose message went closes
    });
    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(kSpinMs);
    connect(m_spinTimer, &QTimer::timeout, this, [this] {
        bool any = false;
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            if (!it->browser || !it->browser->isVisible() || it->spinRects.isEmpty())
                continue;
            any = true;
            for (const QRect& r : qAsConst(it->spinRects))
                it->browser->viewport()->update(r);
        }
        if (!any)
            m_spinTimer->stop();
    });
    // 2.4 auto-load: two owned timers, nothing else (no delayed functor, no thread).
    m_autoTimer = new QTimer(this);
    m_autoTimer->setSingleShot(true);
    m_autoTimer->setInterval(kAutoDelayMs);
    connect(m_autoTimer, &QTimer::timeout, this, &ChatLinkPreviews::runAutoLoads);
    m_waitTimer = new QTimer(this);
    m_waitTimer->setInterval(kWaitTickMs);
    connect(m_waitTimer, &QTimer::timeout, this, [this] {
        // A visible slot waits (for us, a sender, a budget token, a busy site): its time or the sender may have changed.
        // 2.5: the sender's card pictures on screen are asked for here, at most one a tick per connection.
        if (m_down || m_stopped || (!m_waitSeen && m_lpxPictures.isEmpty())) {
            m_waitTimer->stop();
            return;
        }
        drainLpxPictures();
        if (!m_waitSeen)
            return;
        m_waitSeen = false;
        m_present.clear();
        updateVisibleViewports();
    });
    pruneLinkCache(); // what earlier sessions left over the cap
    setUpYouTube();   // 2.5 YouTube player
}

// ---- 2.5 YouTube player -------------------------------------------------------------------------------------------

void ChatLinkPreviews::setUpYouTube()
{
    YouTubePlayer::Host host;
    host.locate = [this](const QString& slotId, YouTubePlayer::Spot* spot) { return locateYouTube(slotId, spot); };
    host.changed = [this](const QString& slotId) { repaintSlot(slotId); };
    host.openUrl = [this](const QUrl& url) { openInBrowser(url); };
    host.pauseOthers = [this] {
        if (m_chat)
            m_chat->pauseAllPlayback(); // TS Media's videos, audio cards and the viewer
    };
    host.muted          = [this] { return m_chat && m_chat->m_media && m_chat->m_media->isMuted(); };
    host.scrollIntoView = [this](const QString& slotId) { scrollSlotIntoView(slotId); };
    // Nothing is loaded here: the helper is extracted and loaded inside the first Play (PlayerBackend::load).
    std::unique_ptr<PlayerBackend> backend;
    if (ytbackend::helperBuiltIn())
        backend.reset(new HelperBackend());
    m_yt = new YouTubePlayer(std::move(host), std::move(backend), this);
    // A TS Media video, audio card or voice message starts: YouTube pauses (one at a time).
    connect(m_chat, &ChatIntegration::playbackStarted, this, [this] {
        if (m_yt)
            m_yt->playbackStarted();
    });
    // Helpers of earlier versions go (a loaded one can't: next time); the switch off: WebView2's folder goes.
    if (ytbackend::helperBuiltIn())
        ytbackend::deleteStaleHelpers(ytbackend::playerDir(), ytbackend::helperFileName(ytbackend::embeddedHelper(), ytbackend::archName()));
    m_yt->settingsChanged();
}

bool ChatLinkPreviews::locateYouTube(const QString& slotId, void* out) const
{
    auto* spot = static_cast<YouTubePlayer::Spot*>(out);
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        QTextBrowser* browser = it->browser;
        SlotRef       ref;
        if (!browser || !refOfSlot(browser, slotId, &ref) || !ref.slot.isCard())
            continue;
        auto*                     self  = const_cast<ChatLinkPreviews*>(this);
        const PreviewStyle        style = styleOf(browser);
        const linkstate::Decision d     = self->decisionFor(ref, false);
        const linkcard::Card      card  = self->cardFor(ref, d, style);
        if (card.look != linkcard::Look::YouTube)
            return false;
        const linkcard::Layout l     = linkcard::layout(card, style);
        const PreviewScale     scale = m_chat->scaleOf(browser);
        const QRectF           rect  = objectRect(browser, ref.slot.object, ref.slot.size);
        const QRectF           thumb = scale.toLogical(l.thumb).translated(rect.topLeft());
        QWidget*               vp    = browser->viewport();
        QWidget*               top   = vp->window();
        const qreal            dpr   = top->devicePixelRatioF() > 0 ? top->devicePixelRatioF() : 1.0;
        void* const            hwnd  = reinterpret_cast<void*>(top->winId());
        const QRect            owner = ytbackend::clientRectOnScreen(hwnd);
        // Logical px in the top-level widget x its dpr = its client area's device px; ClientToScreen: the screen's.
        const auto device = [&](const QRectF& r) {
            const QPointF a = QPointF(vp->mapTo(top, QPoint(0, 0))) + r.topLeft();
            return QRect(qRound(a.x() * dpr) + owner.left(), qRound(a.y() * dpr) + owner.top(), qRound(r.width() * dpr), qRound(r.height() * dpr));
        };
        spot->viewport  = vp;
        spot->thumb     = device(thumb);
        spot->view      = device(QRectF(vp->rect()));
        spot->owner     = owner;
        spot->ownerHwnd = hwnd;
        spot->thumbDu   = l.thumb.size();
        spot->s         = scale.s();
        spot->popupS    = previewscale::fontScale20(previewscale::chatPxOf(browser->document()), Settings::instance().previewScale) / 20.0;
        spot->dpr       = browser->devicePixelRatioF();
        spot->sysScale  = browser->logicalDpiY() / 96.0;
        spot->title     = card.title;
        spot->shown     = browser->isVisible() && !top->isMinimized();
        return true;
    }
    return false;
}

void ChatLinkPreviews::repaintSlot(const QString& slotId)
{
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        SlotRef ref;
        if (it->browser && refOfSlot(it->browser, slotId, &ref) && ref.slot.isCard())
            repaintCard(it->browser, ref.slot.name);
    }
}

void ChatLinkPreviews::scrollSlotIntoView(const QString& slotId)
{
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        QTextBrowser* browser = it->browser;
        SlotRef       ref;
        if (!browser || !refOfSlot(browser, slotId, &ref) || !ref.slot.isCard())
            continue;
        const PreviewStyle     style = styleOf(browser);
        const linkcard::Card   card  = cardFor(ref, decisionFor(ref, false), style);
        const linkcard::Layout l     = linkcard::layout(card, style);
        const QRectF thumb = m_chat->scaleOf(browser).toLogical(l.thumb).translated(objectRect(browser, ref.slot.object, ref.slot.size).topLeft());
        const QRectF view  = QRectF(browser->viewport()->rect());
        QScrollBar*  bar   = browser->verticalScrollBar();
        if (thumb.top() < view.top())
            bar->setValue(bar->value() + qFloor(thumb.top() - view.top()));
        else if (thumb.bottom() > view.bottom())
            bar->setValue(bar->value() + qCeil(thumb.bottom() - view.bottom()));
        return;
    }
}

ChatLinkPreviews::~ChatLinkPreviews()
{
    m_down = true;
    // Every fetch canceled and its worker joined before anything else goes (done already when the plugin
    // unloads: prepareShutdown()).
    prepareShutdown();
    if (m_cursorSet && m_hover.browser)
        m_hover.browser->viewport()->setCursor(m_savedCursor);
    restoreAll();
}

void ChatLinkPreviews::prepareShutdown()
{
    // 2.5 YouTube player first: every timer, filter and connection of it goes, then the helper closes every player
    // synchronously (its pinned image stays, inert).
    if (m_yt)
        m_yt->shutdown();
    // Website fetches canceled and their threads joined, the poster worker joined. The cards stay until the
    // destructor gives every document back; nothing loads any more.
    m_stopped = true;
    // 2.4 auto-load: nothing is queued or waits any more, and the hub, our pipeline and the peer directory (they
    // outlive us) call us no more.
    m_autoTimer->stop();
    m_waitTimer->stop();
    m_autoWanted.clear();
    m_autoRuns.clear();
    m_autoHeld.clear();
    m_pipelineWaits.clear();
    m_present.clear();
    m_budget.clear();
    m_lpxPictures.clear(); // 2.5: nothing more is asked of Core
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        it->quiet.clear(); // 2.5: quiet slots load no more
    if (m_hub)
        disconnect(m_hub.data(), nullptr, this, nullptr);
    if (m_pipeline)
        disconnect(m_pipeline.data(), nullptr, this, nullptr);
    if (m_peers)
        disconnect(m_peers.data(), nullptr, this, nullptr);
    m_hub      = nullptr;
    m_pipeline = nullptr;
    m_peers    = nullptr;
    if (m_fetcher)
        m_fetcher->shutdown();
    m_pool.clear();
    m_pool.waitForDone();
    for (auto it = m_local.begin(); it != m_local.end();) {
        if (it->state == linkstate::Local::Loading || it->state == linkstate::Local::Cooling) // 2.5: no busy wait goes on
            it = m_local.erase(it);
        else
            ++it;
    }
}

QString ChatLinkPreviews::linksDir() const
{
    return linkpreview::linksCacheDir(m_core); // the sender pipeline downloads there too (2.4 auto-load)
}

qint64 ChatLinkPreviews::linkCacheBytes()
{
    // About 200 MB, a quarter of a smaller media cache, never below what one click may load (a 50 MB video).
    const qint64 mb = qBound<qint64>(100, static_cast<qint64>(Settings::instance().cacheLimitMB) / 4, 200);
    return mb * 1024 * 1024;
}

void ChatLinkPreviews::pruneLinkCache()
{
    linkpreview::Fetcher::pruneCache(linksDir(), linkCacheBytes());
    forgetMissingFiles();
}

void ChatLinkPreviews::forgetMissingFiles()
{
    // A link loaded here whose files left the cache (pruned, cleared): a click loads it again.
    for (auto it = m_local.begin(); it != m_local.end();) {
        bool gone = false;
        if (it->state == linkstate::Local::Loaded) {
            if (!it->mediaKey.isEmpty()) {
                const MediaEntry* e = m_core->entry(it->mediaKey);
                gone                = !e || e->state != MediaState::Ready || !QFileInfo::exists(e->localPath);
                if (gone)
                    m_watched.remove(it->mediaKey);
            } else {
                gone = linkstate::localFilesGone(*it); // a YouTube thumbnail, or (2.5) a card's picture
            }
        }
        if (gone) {
            // Its decoded pictures go too: a card never keeps a picture (or an empty box) whose file is gone.
            const QString picture = !it->card.pictureFile.isEmpty() ? it->card.pictureFile : it->thumbnailFile;
            if (!picture.isEmpty()) {
                const QString prefix = picture + QLatin1Char('@');
                for (int i = m_thumbOrder.size() - 1; i >= 0; --i) {
                    if (m_thumbOrder.at(i).startsWith(prefix)) {
                        m_thumbs.remove(m_thumbOrder.at(i));
                        m_thumbOrder.removeAt(i);
                    }
                }
            }
            it = m_local.erase(it);
        } else {
            ++it;
        }
    }
}

bool ChatLinkPreviews::enabled()
{
    const Settings& s = Settings::instance();
    return s.inlinePreviews && s.showLinkPreviews;
}

void ChatLinkPreviews::setStore(linkpreview::PreviewStore* store)
{
    if (m_store)
        disconnect(m_store.data(), nullptr, this, nullptr);
    m_store = store;
    // Direct (the store lives on our thread; onAnnounced only notes the link and starts the refresh timer): a queued
    // linkpreview::Announcement would need Qt's metatype registry, which outlives this DLL (see the fetcher above).
    if (store)
        connect(store, &linkpreview::PreviewStore::announced, this, &ChatLinkPreviews::onAnnounced, Qt::DirectConnection);
    scheduleRefresh();
}

void ChatLinkPreviews::setHub(linkpreview::Hub* hub, peers::PeerDirectory* peers)
{
    if (m_hub)
        disconnect(m_hub.data(), nullptr, this, nullptr);
    if (m_pipeline)
        disconnect(m_pipeline.data(), nullptr, this, nullptr);
    if (m_peers)
        disconnect(m_peers.data(), nullptr, this, nullptr);
    m_hub      = hub;
    m_pipeline = hub ? hub->pipeline() : nullptr;
    m_peers    = peers;
    setStore(hub ? hub->store() : nullptr);
    if (m_down || m_stopped)
        return;
    // Queued, member slots with this as context: nothing of ours is called once we are gone.
    if (m_hub)
        connect(m_hub.data(), &linkpreview::Hub::skipped, this, &ChatLinkPreviews::onSkipped, Qt::QueuedConnection);
    if (m_pipeline)
        connect(m_pipeline.data(), &linkpreview::SenderPipeline::ownLinkChanged, this, &ChatLinkPreviews::onOwnLinkChanged, Qt::QueuedConnection);
    if (m_peers)
        connect(m_peers.data(), &peers::PeerDirectory::presenceChanged, this, &ChatLinkPreviews::onPresenceChanged, Qt::QueuedConnection);
}

void ChatLinkPreviews::setLoader(std::function<void(const linkpreview::Target&, const linkpreview::Caps&, const QString&)> loader)
{
    m_loader = std::move(loader);
}

qint64 ChatLinkPreviews::maxBytesFor(linkpreview::Kind kind)
{
    return linkpreview::autoload::clickCap(kind);
}

ChatLinkPreviews::View* ChatLinkPreviews::viewOf(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

void ChatLinkPreviews::track(QTextBrowser* browser, View& view)
{
    if (view.tracked)
        return;
    view.tracked = true;
    connect(browser, &QObject::destroyed, this, [this, browser] { m_views.remove(browser); });
}

QString ChatLinkPreviews::newCardName()
{
    return linkdoc::cardPrefix() + m_tag + QString::number(++m_serial);
}

QStringList ChatLinkPreviews::excludedHrefs() const
{
    const Settings& s = Settings::instance();
    return QStringList{s.pluginDownloadUrl, QString::fromLatin1(Settings::defaultDownloadUrl)};
}

QString ChatLinkPreviews::messageKeyOf(QTextBrowser* browser, int block, ChatTextReactions::KeyDays* days, QString* sender) const
{
    // The one message key of 2.4: textreactdoc::localKeyOf over the chat document, with the chat's day lines as
    // the text reactions read them (ChatTextReactions::keyOf). The text index the hub resolves an LP against is
    // built from the same entries, so the store finds the announcement under this key. Our slots never change it
    // (textreactdoc::identify skips them; tst_linkui linkPreviewKeepsTheMessageKey). A message the text
    // reactions haven't read (yet, or again since our slot went in) reads the chat's days: once per pass with
    // days (800 links in a 4000-line chat would read them 800 times: over a second of a frozen TeamSpeak).
    // 2.4 auto-load: the sender is the message header's uid as the text reactions read it (every LP and LPNO is
    // matched against that very uid).
    if (sender)
        sender->clear();
    return m_chat->m_textReactions ? m_chat->m_textReactions->keyOf(browser, block, days, sender) : QString();
}

QString ChatLinkPreviews::slotIdOf(QTextBrowser* browser, int block, const QString& urlKey, ChatTextReactions::KeyDays* days, QString* senderUid) const
{
    const QString key = messageKeyOf(browser, block, days, senderUid);
    if (!key.isEmpty())
        return key + QLatin1Char('|') + urlKey;
    // No message key (no timestamps, a server chat): the message's text, as text reactions read it.
    const textreactdoc::Identity id = textreactdoc::identify(browser->document()->findBlockByNumber(block));
    if (senderUid)
        *senderUid = id.uid;
    const QString base = id.hash.isEmpty() ? QString::fromLatin1("b%1").arg(block) : id.hash + QLatin1Char('@') + QString::number(id.seconds);
    return base + QLatin1Char('|') + urlKey;
}

void ChatLinkPreviews::fillMessage(SlotRef* ref, linkpreview::autoload::SenderQueue* queue, const ChatContext& ctx) const
{
    QString hash;
    qint64  t = 0;
    ref->timeSec   = !ref->messageKey.isEmpty() && textreact::parseLocalKey(ref->messageKey, &hash, &t) ? t : -1;
    // The sender makes one preview at a time across their messages: this link waits for theirs before it too. 2.5: per
    // link class, only links the sender makes count, and one past their plugin's per-minute intake waits 0.
    ref->budgetSec = queue ? queue->add(ref->senderUid, ref->timeSec, ref->target, senderMakes(*ref, ctx)) : 0;
}

bool ChatLinkPreviews::senderMakes(const SlotRef& ref, const ChatContext& ctx) const
{
    // Without a peer directory (tools) or a connection: as 2.4.0 (the plain wait, bounded anyway).
    if (!m_peers || !ctx.sch || ref.senderUid.isEmpty())
        return true;
    const char* cap = linkpreview::autoload::capFor(ref.target.cls);
    if (!cap)
        return false;
    const peers::PeerDirectory::CapState state = m_peers->capabilityState(ctx.sch, ref.senderUid, cap);
    return state == peers::PeerDirectory::CapState::Yes || state == peers::PeerDirectory::CapState::Checking
           || state == peers::PeerDirectory::CapState::Unknown;
}

// ---- what a slot shows ----------------------------------------------------------------------------------------

linkstate::LocalState ChatLinkPreviews::localOf(const QString& urlKey)
{
    auto it = m_local.find(urlKey);
    if (it == m_local.end())
        return {};
    if (it->state == linkstate::Local::Loaded && !it->mediaKey.isEmpty()) {
        const MediaEntry* e = m_core->entry(it->mediaKey);
        if (!e || e->state != MediaState::Ready) {
            // Its file left the cache (cleared, evicted): a click loads it again.
            m_watched.remove(it->mediaKey);
            m_local.erase(it);
            return {};
        }
        m_watched.insert(it->mediaKey);
    }
    return it.value();
}

bool ChatLinkPreviews::serverFilesOf(const linkpreview::Announcement& stored, ServerFiles* files) const
{
    // The hub stores every announcement with its server and channel; older or odd ones fall back to the
    // connection's server and the sender's channel now, else ours (a channel message's files are in its channel).
    QString serverUid;
    quint64 channel = 0;
    if ((stored.serverUid.isEmpty() || stored.channelId == 0) && ts3::isConnected(stored.sch)) {
        serverUid          = ts3::serverUid(stored.sch);
        const anyID sender = stored.senderUid.isEmpty() ? anyID(0) : ts3::clientIdByUid(stored.sch, stored.senderUid);
        uint64      c      = 0;
        if (sender && ts3::funcs.getChannelOfClient && ts3::funcs.getChannelOfClient(stored.sch, sender, &c) == ERROR_ok)
            channel = c;
        if (channel == 0)
            channel = ts3::ownChannel(stored.sch);
    }
    const linkpreview::Announcement a = linkstate::withFileBrowser(stored, serverUid, channel);
    MediaLink                       main;
    if (!linkstate::serverLink(a, &main)) // the sender's linkFor(): one Core entry per file, whoever asks
        return false;
    files->main         = main;
    files->hasMedia     = !a.mediaPath.isEmpty() && main.size > 0;
    files->announcement = a;
    return true;
}

linkstate::ServerState ChatLinkPreviews::serverOf(const QString& messageKey, const linkpreview::Target& target, bool ensure)
{
    linkstate::ServerState s;
    if (!m_store || messageKey.isEmpty())
        return s;
    const linkpreview::Announcement* a = m_store->find(messageKey, target.key);
    // 2.5: an LP only under a 2.4 link, an LPX only under a link of the new classes (a web page's only while cards for
    // other web pages are on).
    if (!a || (a->extended ? !linkstate::lpxFits(a->kind, target, Settings::instance().showWebCards) : !linkstate::lpFits(a->kind, target)))
        return s;
    if (a->extended && a->kind == linkpreview::Kind::Card) {
        // A card: its text is the preview; its picture (when it has one) comes from the TeamSpeak server.
        s.announced = true;
        s.extended  = true;
        s.kind      = linkpreview::Kind::Card;
        s.card      = a->card;
        s.sensitive = a->sensitive;
        ServerFiles files;
        if (!a->previewPath.isEmpty() && serverFilesOf(*a, &files)) {
            s.mainKey = files.main.key();
            m_serverKeys.insert(s.mainKey);
            m_watched.insert(s.mainKey);
            linkstate::readEntry(&s, m_core->entry(s.mainKey));
        }
        return s;
    }
    ServerFiles files;
    if (!serverFilesOf(*a, &files))
        return s;
    s.announced = true;
    s.hasMedia  = files.hasMedia;
    s.mainKey   = files.main.key();
    s.extended  = a->extended;
    s.kind      = a->kind;
    s.sensitive = a->sensitive;
    m_serverKeys.insert(s.mainKey);
    if (ensure && !m_ensured.contains(s.mainKey) && !a->extended) {
        m_ensured.insert(s.mainKey);
        // From the TeamSpeak server, checked against its SHA-256: the preview always, the full file by the user's
        // automatic download rules (the hub asked already when the announcement came; Core knows it then). 2.5: an
        // LPX's files are asked for only while its slot is on screen (renderVisible, paced: m_lpxPictures).
        linkpreview::requestFiles(m_core, files.announcement);
    }
    m_watched.insert(s.mainKey);
    linkstate::readEntry(&s, m_core->entry(s.mainKey));
    return s;
}

linkstate::LocalState ChatLinkPreviews::slotLocal(const SlotRef& ref, linkstate::LocalState local)
{
    using linkstate::Local;
    // Our own message while our pipeline has the link (queued or fetching): "Making the preview…", whatever an earlier
    // load of that link here ended with (the state is shared by every message showing the link).
    if ((local.state == Local::Failed || local.state == Local::TooLarge || local.state == Local::OverAutoLimit) && m_pipeline
        && m_pipeline->pending(ref.target.key)) {
        const linkpreview::autoload::Input in = inputFor(ref);
        if (in.own && in.ownRecord == linkpreview::autoload::OwnRecord::Pending)
            return {};
    }
    // Larger than a click may load (an automatic load's answer, our pipeline's size): "Too large", Open in browser.
    if (local.state == Local::Idle || local.state == Local::OverAutoLimit) {
        const qint64 known = local.state == Local::OverAutoLimit ? local.size : m_sizeHints.value(ref.target.key, -1);
        const qint64 click = linkpreview::autoload::mediaCapOf(linkpreview::autoload::clickCaps(ref.target), local.kind);
        if (known > click && (ref.target.kind != linkpreview::Kind::Card || local.kind != linkpreview::Kind::None)) {
            local.state = Local::TooLarge;
            local.size  = known;
        }
    }
    // 2.5: a busy site's time has come: a candidate again (its rounds kept in m_local).
    if (linkstate::coolingDue(local, wallMs()))
        local.state = Local::Idle;
    // 2.5 (6.4): a link nobody loaded here whose site is cooling (another link of it was told to wait) shows the wait;
    // a site behind a bot check the neutral error (a click wouldn't help either). Only while links load by themselves.
    if (local.state == Local::Idle && Settings::instance().autoLoadLinkPreviews) {
        qint64 ms = 0;
        const linkpreview::HostGate::Verdict v = gateOf(ref.target, &ms);
        local = linkstate::underGate(local, v, ms, wallMs());
    }
    return local;
}

linkstate::Context ChatLinkPreviews::contextFor(const SlotRef& ref) const
{
    const Settings&    s = Settings::instance();
    linkstate::Context c;
    c.autoOn   = s.autoLoadLinkPreviews;
    c.webCards = s.showWebCards;
    if (m_hub && !ref.messageKey.isEmpty() && m_hub->isSkipped(ref.messageKey, ref.target.key)) {
        const linkpreview::SkipReason why = m_hub->skipReason(ref.messageKey, ref.target.key);
        c.senderNothing                   = why == linkpreview::SkipReason::Nothing || why == linkpreview::SkipReason::CrossSite;
    }
    // 2.5 (design2 2.7): the sender found a share card (not media) while cards for other web pages are off here: that
    // link has nothing to show here either, and is never loaded by itself to learn it again.
    if (!c.webCards && m_store && !ref.messageKey.isEmpty() && linkpreview::providers::cardsFollowWebRules(ref.target.cls, ref.target.provider)) {
        const linkpreview::Announcement* a = m_store->find(ref.messageKey, ref.target.key);
        if (a && a->extended && a->kind == linkpreview::Kind::Card)
            c.senderNothing = true;
    }
    return c;
}

linkpreview::HostGate::Verdict ChatLinkPreviews::gateOf(const linkpreview::Target& target, qint64* ms) const
{
    qint64 left = 0;
    if (ms)
        *ms = 0;
    const std::shared_ptr<linkpreview::HostGate> gate = m_fetcher ? m_fetcher->gate() : std::shared_ptr<linkpreview::HostGate>();
    if (!gate || !target.isValid())
        return linkpreview::HostGate::Verdict::Go;
    const linkpreview::providers::FirstRequest first = linkpreview::providers::firstRequestOf(target);
    if (first.host.isEmpty())
        return linkpreview::HostGate::Verdict::Go;
    linkpreview::HostGate::Verdict v = gate->state(first.host, first.lane, first.firstSegment, &left);
    if (v == linkpreview::HostGate::Verdict::Walled) {
        // A walled Klipy GIF page loads through Klipy's oEmbed (the Fetcher's admit): that request's state decides.
        const linkpreview::providers::FirstRequest fallback = linkpreview::providers::wallFallbackOf(target);
        if (!fallback.host.isEmpty())
            v = gate->state(fallback.host, fallback.lane, fallback.firstSegment, &left);
    }
    if (ms)
        *ms = left;
    // Wait only spaces requests (the Fetcher's queue handles it): for the chat that is Go.
    return v == linkpreview::HostGate::Verdict::Wait ? linkpreview::HostGate::Verdict::Go : v;
}

QString ChatLinkPreviews::shownHost(const SlotRef& ref, const linkstate::LocalState& local) const
{
    // The host a card names is always our own parse of the link, the one a click opens: never anything a page or a
    // sender says. (A web card loaded here after a redirect to another website names that website after it: Card::
    // redirectedTo.)
    Q_UNUSED(local);
    return linkcard::cleanText(ref.target.host, 120);
}

linkstate::Decision ChatLinkPreviews::decisionFor(const SlotRef& ref, bool ensure)
{
    const linkstate::LocalState  local  = slotLocal(ref, localOf(ref.target.key));
    const linkstate::ServerState server = serverOf(ref.messageKey, ref.target, ensure);
    const bool                   hidden = m_hidden.contains(ref.slotId);
    // 2.4 auto-load: what it waits for matters only when nothing else is shown (decide's rule 6).
    linkstate::Wait wait = linkstate::Wait::None;
    const bool      bare = local.state == linkstate::Local::Idle || local.state == linkstate::Local::OverAutoLimit
                      || (local.state == linkstate::Local::Loaded && local.mediaKey.isEmpty() && local.card.isEmpty()
                          && ref.target.kind != linkpreview::Kind::YouTube);
    if (!hidden && bare && !serverUsable(server)) {
        // A sender's preview that came and failed for good (linkstate::serverFailed) is waited for no more: the
        // website is the only source left (race rule R13).
        if (!server.announced)
            wait = toLinkstate(waitOf(ref));
        // Drawn as Loading, runAutoLoads starts it; one it held back (no free slot, the sender's load or budget) stays a
        // click card until its turn, so no card turns from the ring to "Load preview" and back.
        if (wait == linkstate::Wait::None && !m_autoHeld.contains(ref.slotId) && autoEligible(ref, local, server))
            wait = linkstate::Wait::Auto;
    }
    linkstate::Decision d = linkstate::decide(ref.target, hidden, local, server, wait, contextFor(ref));
    d.index               = ref.index;
    return d;
}

linkcard::Card ChatLinkPreviews::cardFor(const SlotRef& ref, const linkstate::Decision& d, const PreviewStyle& style)
{
    Q_UNUSED(style);
    linkcard::Card c;
    c.look      = d.look;
    c.kind      = ref.target.kind;
    c.cls       = ref.target.cls;
    c.provider  = ref.target.provider;
    c.slotIndex = d.index;
    const linkstate::LocalState l = slotLocal(ref, m_local.value(ref.target.key));
    c.host                        = shownHost(ref, l);
    // 2.5: a web link whose load found media is drawn as that media's kind; a share's too (a video share over the limit).
    if (linkpreview::providers::cardsFollowWebRules(ref.target.cls, ref.target.provider) && l.kind != linkpreview::Kind::None && l.kind != linkpreview::Kind::Card)
        c.kind = l.kind;
    switch (d.look) {
    case linkcard::Look::Loading:
        c.progress = d.wait == linkstate::Wait::None ? l.progress : -1.0;
        c.waiting  = linkstate::waitingShown(d); // a sender's preview that is here already: loading, not waiting
        if (c.waiting == linkcard::Waiting::Host) {
            // A busy site: the time left, and which site asked (its registrable domain).
            c.retryInMs = qMax<qint64>(0, l.retryAtMs - wallMs());
            c.busySite  = linkpreview::net::registrableDomain(linkpreview::providers::firstRequestOf(ref.target).host);
        }
        break;
    case linkcard::Look::Click:
        c.size          = shownSize(ref, l); // above the automatic limit: "Video · 38 MB"
        c.overAutoLimit = l.state == linkstate::Local::OverAutoLimit || c.size > 0;
        if (c.overAutoLimit) // the site didn't say its size: "Video · over 15 MB"
            c.autoLimit = linkpreview::autoload::mediaCapOf(linkpreview::autoload::autoCaps(ref.target, Settings::instance(), contextOf(ref.browser).serverUid),
                                                            l.kind);
        break;
    case linkcard::Look::Error:
        c.error   = linkcard::cleanText(l.error, 160);
        c.nothing = d.nothing;
        break;
    case linkcard::Look::Embed: {
        // The card's text, from our own load or the sender's LPX, cleaned again (it is a website's, or another
        // client's).
        linkpreview::CardData data;
        if (d.fromServer) {
            const linkstate::ServerState s = serverOf(ref.messageKey, ref.target, false);
            // No picture, or one that can never come here (failed for good): a text card, never an empty box.
            data                           = linkstate::serverCardData(s);
            c.fromSender                   = true;
            const MediaEntry* e            = s.mainKey.isEmpty() ? nullptr : m_core->entry(s.mainKey);
            c.pictureLoading = data.layout != linkpreview::CardLayout::None && (!e || e->state == MediaState::Idle || e->state == MediaState::Queued
                                                                                || e->state == MediaState::Downloading);
            if (s.mainKey.isEmpty() || (e && e->state == MediaState::Failed))
                c.pictureLoading = false;
        } else {
            data = l.card;
        }
        // A web page's own name only when it matches the host shown (never a well-known site's look); a site card's
        // line is its brand (linkcard). A card from another website (a redirect) names the link's own host first and
        // that website after it, without its name or colour.
        const bool crossSite = !d.fromServer && l.state == linkstate::Local::Loaded && !l.finalHost.isEmpty();
        c.data               = linkstate::shownCardData(data, ref.target.cls, c.host, crossSite, ref.target.provider);
        c.revealed           = Settings::instance().revealSpoilers || m_revealed.contains(ref.slotId);
        if (crossSite)
            c.redirectedTo = linkcard::cleanText(l.finalHost, 120);
        break;
    }
    case linkcard::Look::TooLarge:
        c.size = l.size;
        break;
    case linkcard::Look::YouTube:
    case linkcard::Look::Poster:
        if (d.fromServer && m_store) {
            if (const linkpreview::Announcement* a = m_store->find(ref.messageKey, ref.target.key)) {
                c.title      = linkcard::cleanText(a->title, 200);
                c.author     = linkcard::cleanText(a->author, 100);
                c.durationMs = qMax<qint64>(0, a->durationMs);
            }
        } else {
            c.title  = linkcard::cleanText(l.title, 200);
            c.author = linkcard::cleanText(l.author, 100);
        }
        if (d.look == linkcard::Look::YouTube && m_yt) {
            // 2.5 YouTube player: the player's state on this card (loads nothing).
            const YouTubePlayer::Card yt = m_yt->cardOf(ref.slotId, ref.target.videoId);
            c.player                     = static_cast<linkcard::Player>(static_cast<int>(yt.state));
            c.playable                   = yt.playable && ytpage::validId(ref.target.videoId);
            c.note                       = yt.note;
            c.resumeMs                   = yt.resumeMs;
        }
        break;
    default:
        break;
    }
    return c;
}

QImage ChatLinkPreviews::thumbnailFor(const SlotRef& ref, const linkstate::Decision& d, const QSize& pixels)
{
    if (pixels.isEmpty())
        return {};
    if (d.fromServer) {
        const linkstate::ServerState s = serverOf(ref.messageKey, ref.target, false);
        if (s.mainKey.isEmpty())
            return {};
        return m_core->still(s.mainKey, pixels).image; // Core decodes it (large ones on a worker)
    }
    const linkstate::LocalState l    = m_local.value(ref.target.key);
    const QString               file = d.look == linkcard::Look::Embed ? l.card.pictureFile : l.thumbnailFile; // 2.5: a card's picture
    if (file.isEmpty())
        return {};
    const QString cacheKey = file + QLatin1Char('@') + QString::number(pixels.width()) + QLatin1Char('x') + QString::number(pixels.height());
    if (m_thumbs.contains(cacheKey))
        return m_thumbs.value(cacheKey);
    QImage image;
    // 2.5: a website's picture is decoded by its bytes only, the format forced (safeimage.h).
    const std::unique_ptr<QImageReader> safe   = safeimage::reader(file, safeimage::Source::Website);
    QImageReader&                       reader = *safe;
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    // A website's picture: only a sane size is decoded (a thumbnail is about 1280 x 720).
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() <= 8LL * 1000 * 1000) {
        reader.setScaledSize(QSizeF(size).scaled(QSizeF(pixels), Qt::KeepAspectRatioByExpanding).toSize().boundedTo(size));
        image = reader.read();
    }
    m_thumbs.insert(cacheKey, image);
    m_thumbOrder.append(cacheKey);
    while (m_thumbOrder.size() > kMaxThumbs)
        m_thumbs.remove(m_thumbOrder.takeFirst());
    return image;
}

// ---- reading the chat and keeping its slots ----------------------------------------------------------------------

void ChatLinkPreviews::afterScan(QTextBrowser* browser)
{
    if (!browser || m_down)
        return;
    View& view  = m_views[browser];
    view.browser = browser;
    track(browser, view);
    QTextDocument* doc = browser->document();
    if (view.document != doc) {
        // TeamSpeak swapped the chat's document: the old one gets its text back (it may be shown again).
        if (view.document)
            restore(view);
        view.document = doc;
        view.rendered.clear();
        view.refsValid = false;
        connect(doc, &QTextDocument::contentsChange, this, [this, browser, guard = QPointer<QTextDocument>(doc)](int, int, int) {
            View* v = viewOf(browser);
            if (v && v->document == guard)
                v->refsValid = false;
        });
    }
    reconcile(browser);
    if (m_yt)
        m_yt->relayout(); // 2.5 YouTube player: a message that went closes its player; the anchored one follows
}

void ChatLinkPreviews::reconcile(QTextBrowser* browser)
{
    View* view = viewOf(browser);
    if (!view || !view->document || view->document != browser->document() || m_down)
        return;
    QTextDocument* doc = view->document;
    if (!enabled()) {
        restore(*view);
        return;
    }
    const QVector<linkdoc::Link> links    = linkdoc::findLinks(doc, excludedHrefs());
    const QVector<linkdoc::Slot> existing = linkdoc::allSlots(doc);
    if (links.isEmpty() && existing.isEmpty())
        return;
    const PreviewStyle style = styleOf(browser);
    const PreviewScale scale = m_chat->scaleOf(browser); // 2.5 zoom: slots in du, laid out at the chat's scale

    struct Edit {
        int     position = 0; // where: the link's end (insert), else the picture
        int     type     = 0; // 0 insert, 1 replace, 2 resize, 3 remove
        int     link     = -1;
        QString name;
        QSizeF  size;
    };
    QVector<Edit>              edits;
    QSet<int>                  kept;
    ChatTextReactions::KeyDays days; // the document doesn't change before the edits below
    const bool                        autoOn = Settings::instance().autoLoadLinkPreviews;
    const ChatContext                 ctx    = contextOf(browser);
    linkpreview::autoload::SenderQueue queue; // the senders' links so far (each sender makes them one after the other)
    for (int i = 0; i < links.size(); ++i) {
        const linkdoc::Link& link = links.at(i);
        m_targets.insert(link.target.key, link.target);
        SlotRef ref;
        ref.browser    = browser;
        ref.block      = link.block;
        ref.target     = link.target;
        ref.linkStart  = link.start;
        ref.linkEnd    = link.end;
        ref.index      = link.index;
        ref.messageKey = messageKeyOf(browser, link.block, &days, &ref.senderUid);
        ref.slotId     = ref.messageKey.isEmpty() ? slotIdOf(browser, link.block, link.target.key, &days, &ref.senderUid)
                                                  : ref.messageKey + QLatin1Char('|') + link.target.key;
        fillMessage(&ref, &queue, ctx);
        // 2.5 (R22): an expired Discord attachment shows nothing, at once (no request, no wait).
        expiredCheck(link.target);
        // 2.4 auto-load, before the decision: our own pipeline's download (the signal may have come before the slot
        // did), then the links folder (no request; every link of the chat, so a slot has its final size at once).
        // 2.5: the cache answers for every class (cards and "nothing to show" too), also with automatic loading off.
        const bool own = isOwn(ref, ctx);
        if (own) {
            applyOwnRecord(link.target.key);
            showHeldCard(link.target);
        }
        if ((autoOn || link.target.isNewClass()) && !m_local.contains(link.target.key) && !m_cacheMiss.contains(link.target.key)
            && !serverOf(ref.messageKey, link.target, false).announced && !(m_pipeline && m_pipeline->fetching(link.target.key)))
            tryCache(link.target, own);
        const linkstate::Decision d     = decisionFor(ref, true);
        const linkdoc::Slot       slot  = linkdoc::slotAfter(doc->findBlockByNumber(link.block), link);
        if (!d.media && d.look == linkcard::Look::Absent)
            continue; // 2.5: no slot (an existing one goes below: it isn't kept); a quiet slot is tracked by slotsOf
        QString                   name;
        QSizeF                    size;
        if (d.media) {
            name = linkdoc::mediaPrefix() + d.mediaKey;
            if (slot.exists() && slot.name == name) {
                kept.insert(slot.object); // ChatIntegration keeps its size and picture
                continue;
            }
            size = m_chat->linkPreviewObject(browser, d.mediaKey);
        }
        if (!d.media || size.isEmpty()) {
            linkstate::Decision card = d;
            card.media               = false;
            const QSizeF want        = QSizeF(cardSize(cardFor(ref, card, style), style));
            if (slot.exists() && slot.isCard()) {
                kept.insert(slot.object);
                if (slot.size != want)
                    edits.append({slot.object, 2, i, slot.name, want});
                continue;
            }
            name = newCardName();
            size = want;
            doc->addResource(QTextDocument::ImageResource, QUrl(name), blankPicture()); // drawn right before the chat paints
        }
        if (slot.exists()) {
            kept.insert(slot.object);
            edits.append({slot.object, 1, i, name, size});
        } else {
            edits.append({link.end, 0, i, name, size});
        }
    }
    for (const linkdoc::Slot& s : existing) {
        if (!kept.contains(s.object))
            edits.append({s.object, 3, -1, s.name, QSizeF()});
    }
    if (edits.isEmpty())
        return;
    // Back to front: earlier positions stay valid.
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.position > b.position; });
    // The reader's place: the line at the view's top stays where it is on screen when a slot grows or shrinks
    // (a card that becomes the link's media must not move what is read), the bottom stays the bottom.
    const chatscroll::TextPlace place       = chatscroll::captureText(browser);
    const bool                  wasMutating = m_chat->m_mutating;
    m_chat->m_mutating                      = true; // our own edits: no rescan; the layout fits what it must
    bool        edited                      = false;
    QStringList gone;
    {
        QTextCursor batch(doc);
        batch.beginEditBlock();
        for (const Edit& e : qAsConst(edits)) {
            switch (e.type) {
            case 0:
                edited = linkdoc::insertSlot(doc, links.at(e.link), e.name, e.size, scale) || edited;
                break;
            case 1: {
                QTextCursor c(doc);
                c.setPosition(e.position);
                c.setPosition(e.position + 1, QTextCursor::KeepAnchor);
                const QString was = c.charFormat().toImageFormat().name();
                if (linkdoc::replaceObject(doc, e.position, e.name, e.size, scale)) {
                    edited = true;
                    if (was.startsWith(linkdoc::cardPrefix()))
                        gone << was;
                }
                break;
            }
            case 2:
                edited = linkdoc::resizeObject(doc, e.position, e.size, scale) || edited;
                break;
            case 3:
                if (linkdoc::removeSlot(doc, e.position)) {
                    edited = true;
                    if (e.name.startsWith(linkdoc::cardPrefix()))
                        gone << e.name;
                }
                break;
            default:
                break;
            }
        }
        batch.endEditBlock();
    }
    m_chat->m_mutating = wasMutating;
    for (const QString& name : qAsConst(gone)) {
        doc->addResource(QTextDocument::ImageResource, QUrl(name), blankPicture()); // its memory goes
        if (ChatScale* scaler = m_chat->chatScale(browser))
            scaler->forget(name);
        view->rendered.remove(name);
    }
    view->refsValid = false;
    if (!edited)
        return;
    if (!m_chat->m_scaling) // 2.5 zoom: inside a scale pass the place comes back once, at its end
        chatscroll::restoreText(browser, place);
    if (m_chat->m_replies)
        m_chat->m_replies->documentEdited(browser); // positions inside the blocks moved
    m_chat->scheduleVisibilityUpdate();               // a link's media came or went (GIFs, players)
    browser->viewport()->update();
}

void ChatLinkPreviews::restore(View& view)
{
    if (!view.document)
        return;
    QStringList names;
    // The chat it is shown in keeps the reader's place (the slots above the view's top go).
    QTextBrowser* shown = view.browser && view.browser->document() == view.document ? view.browser.data() : nullptr;
    const chatscroll::TextPlace place = shown ? chatscroll::captureText(shown) : chatscroll::TextPlace();
    {
        const bool wasMutating = m_chat->m_mutating;
        m_chat->m_mutating     = true;
        layoutformat::MutatingScope mutating;
        QTextCursor                 batch(view.document);
        batch.beginEditBlock();
        linkdoc::removeAll(view.document, &names);
        batch.endEditBlock();
        m_chat->m_mutating = wasMutating;
    }
    if (shown && !names.isEmpty())
        chatscroll::restoreText(shown, place);
    for (const QString& name : qAsConst(names)) {
        if (name.startsWith(linkdoc::cardPrefix()))
            view.document->addResource(QTextDocument::ImageResource, QUrl(name), blankPicture());
    }
    view.rendered.clear();
    view.refsValid = false;
    view.spinRects.clear();
    if (!names.isEmpty() && view.browser) {
        if (m_chat->m_replies)
            m_chat->m_replies->documentEdited(view.browser);
        view.browser->viewport()->update();
    }
}

void ChatLinkPreviews::restoreAll()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        restore(it.value());
}

void ChatLinkPreviews::settingsChanged()
{
    // 2.4 auto-load: switched off (or the previews), its loads stop and their cards are click cards again; a lower
    // limit or the data saver stops the ones that may not go on. Clicked loads are never stopped by it.
    const Settings& s = Settings::instance();
    if (!s.autoLoadLinkPreviews || !enabled()) {
        const QList<QString> automatic = m_autoRuns.keys();
        for (const QString& key : automatic) {
            if (enabled() && m_autoRuns.value(key).clicked)
                continue; // R26: the user asked for it meanwhile
            cancelFetch(key);
        }
        m_autoWanted.clear();
        m_autoHeld.clear();
        m_autoTimer->stop();
    } else {
        limitsChanged();
    }
    if (!s.showWebCards) {
        // R21: cards for other web pages off: their loads stop (clicked ones too: the class is off), their slots go.
        QStringList web;
        for (auto it = m_local.constBegin(); it != m_local.constEnd(); ++it) {
            if (it->state == linkstate::Local::Loading && m_targets.value(it.key()).cls == linkpreview::LinkClass::Web)
                web << it.key();
        }
        for (const QString& key : qAsConst(web))
            cancelFetch(key);
    } else if (!m_webOff.isEmpty()) {
        // On again: share links that stopped at another web page (kWebOff, never cached) follow the normal rules again.
        for (const QString& key : qAsConst(m_webOff)) {
            if (m_local.value(key).state == linkstate::Local::Nothing)
                m_local.remove(key);
        }
        m_webOff.clear();
    }
    if (enabled())
        restartWebCardsLoads(); // review fix: share links loading meanwhile follow the switch as it is now
    reviewOverLimit(); // 8.3
    m_present.clear();
    if (m_yt)
        m_yt->settingsChanged(); // 2.5: the switch or the previews off: the player closes
    if (enabled())
        return; // ChatIntegration scans every chat again (afterScan puts the slots in; on-screen ones start again)
    // Off: no fetch goes on, and every slot goes now (before ChatIntegration's own previews).
    QStringList loading;
    for (auto it = m_local.constBegin(); it != m_local.constEnd(); ++it) {
        if (it->state == linkstate::Local::Loading)
            loading << it.key();
    }
    for (const QString& key : qAsConst(loading))
        cancelFetch(key); // our pipeline's own download is never canceled from here
    m_waitTimer->stop();
    m_pool.clear();
    restoreAll();
}

void ChatLinkPreviews::limitsChanged()
{
    if (m_down || m_stopped)
        return;
    // Each automatic load against the limit of its own chat's server (a server's own data saver), as it started.
    const Settings&      s         = Settings::instance();
    const QList<QString> automatic = m_autoRuns.keys();
    bool                 stopped   = false;
    for (const QString& key : automatic) {
        const AutoRun run = m_autoRuns.value(key);
        if (run.clicked)
            continue; // R26: the user asked for it meanwhile: never stopped by a limit
        const qint64  cap = linkpreview::autoload::autoCap(m_targets.value(key), s, run.serverUid);
        if (cap >= run.maxBytes || (cap > 0 && run.total >= 0 && run.total <= cap))
            continue; // the same or a higher limit, or the file fits the lower one: it goes on
        const bool   over = cap > 0 && (run.total > cap || run.received > cap);
        const qint64 size = run.total;
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 stopped (a lower automatic limit or the data saver)",
                 {ts3::pub(kindText(m_targets.value(key).kind)), ts3::name(m_targets.value(key).host)});
        cancelFetch(key);
        stopped = true;
        if (over) {
            // Larger than the new limit: a click card with its size (no second request to learn it). 2.5: with the chat's
            // server and the cap it hit, so a raised limit makes it a candidate again (8.3).
            linkstate::LocalState l;
            l.state     = linkstate::Local::OverAutoLimit;
            l.size      = size;
            l.serverUid = run.serverUid;
            l.capHit    = cap;
            m_local.insert(key, l);
        }
        // Otherwise (data saver, or a size still unknown) it starts over by the new rules, if at all.
    }
    if (stopped)
        scheduleRefresh(); // its card's size may change (a click card with a size)
    reviewOverLimit(); // 8.3: a raised limit
    updateVisibleViewports();
}

void ChatLinkPreviews::reviewOverLimit()
{
    // 2.4.0 open item 3: a "Picture · 30 MB" card stayed one after the automatic limit went up. Entries the new limit
    // covers go (a normal candidate again: it loads by itself when on screen), and so do our pipeline's size hints.
    bool            changed = false;
    const Settings& s       = Settings::instance();
    for (auto it = m_local.begin(); it != m_local.end();) {
        if (it->state == linkstate::Local::OverAutoLimit) {
            // Judged against the cap that refused it (a page link that answered with media: the media cap).
            const qint64 cap = linkpreview::autoload::overLimitCap(m_targets.value(it.key()), it->kind, s, it->serverUid);
            if (!linkstate::stillOverLimit(it.value(), cap)) {
                it      = m_local.erase(it);
                changed = true;
                continue;
            }
        }
        ++it;
    }
    for (auto it = m_sizeHints.begin(); it != m_sizeHints.end();) {
        const qint64 cap = linkpreview::autoload::autoCap(m_targets.value(it.key()), s, QString());
        if (cap > 0 && it.value() <= cap) {
            it      = m_sizeHints.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        scheduleRefresh();
        updateVisibleViewports();
    }
}

void ChatLinkPreviews::scheduleRefresh()
{
    if (!m_down && !m_refreshTimer->isActive())
        m_refreshTimer->start();
}

// ---- the slots of a chat --------------------------------------------------------------------------------------

QVector<ChatLinkPreviews::SlotRef> ChatLinkPreviews::slotsOf(QTextBrowser* browser) const
{
    auto it = m_views.find(browser);
    if (it == m_views.end() || !it->document || it->document != browser->document())
        return {};
    View& view = const_cast<View&>(it.value());
    if (view.refsValid)
        return view.refs;
    view.refs.clear();
    view.quiet.clear();
    ChatTextReactions::KeyDays         days;  // one reading of the chat's days for every slot
    linkpreview::autoload::SenderQueue queue; // 2.4 auto-load: the senders' links so far, in the chat's order
    const ChatContext                  ctx       = contextOf(browser);
    const QStringList                  excluded  = excludedHrefs();
    const int                          firstRead = qMax(0, view.document->blockCount() - linkdoc::kMaxBlocks);
    for (QTextBlock b = view.document->begin(); b.isValid(); b = b.next()) {
        const QVector<linkdoc::Slot> here  = linkdoc::slotsIn(b);
        // 2.5: the block's selected links, in order: those with a slot (document refs) and those without (quiet refs:
        // tracked by their anchor, so they load lazily and count in their sender's queue like a drawn slot).
        const QVector<linkdoc::Link> links = b.blockNumber() >= firstRead ? linkdoc::linksIn(b, excluded) : QVector<linkdoc::Link>();
        QVector<bool>                used(here.size(), false);
        const auto fill = [&](SlotRef& ref, const QString& urlKey) {
            ref.browser    = browser;
            ref.block      = b.blockNumber();
            ref.messageKey = messageKeyOf(browser, ref.block, &days, &ref.senderUid);
            ref.slotId     = ref.messageKey.isEmpty() ? slotIdOf(browser, ref.block, urlKey, &days, &ref.senderUid) : ref.messageKey + QLatin1Char('|') + urlKey;
            fillMessage(&ref, &queue, ctx);
        };
        for (const linkdoc::Link& link : links) {
            SlotRef ref;
            ref.target    = link.target;
            ref.linkStart = link.start;
            ref.linkEnd   = link.end;
            ref.index     = link.index;
            int found     = -1;
            for (int i = 0; i < here.size() && found < 0; ++i) {
                if (!used.at(i) && here.at(i).separator == link.end && here.at(i).urlKey == link.target.key)
                    found = i;
            }
            if (found >= 0) {
                used[found] = true;
                ref.slot    = here.at(found);
                fill(ref, ref.slot.urlKey);
                view.refs.append(ref);
            } else {
                ref.slot.urlKey = link.target.key;
                ref.slot.href   = link.href;
                fill(ref, link.target.key);
                view.quiet.append(ref);
            }
        }
        // Slots whose link isn't selected any more (they go at the next reconcile): as the document has them.
        for (int i = 0; i < here.size(); ++i) {
            if (used.at(i))
                continue;
            const linkdoc::Slot& s = here.at(i);
            SlotRef              ref;
            ref.slot = s;
            // The slot's own link: links of one video share a key (youtu.be/x, www.youtube.com/watch?v=x), so the
            // one remembered for the key may have another host (live test: a www.youtube.com card said youtu.be
            // after a youtu.be link of that video in another chat). The remembered one only if this can't be read.
            ref.target = linkpreview::classifyCached(linkpreview::trimLinkEnd(s.href));
            if (ref.target.key != s.urlKey) {
                const linkpreview::Target known = m_targets.value(s.urlKey);
                if (!known.key.isEmpty())
                    ref.target = known;
            }
            fill(ref, s.urlKey);
            view.refs.append(ref);
        }
    }
    view.refsValid = true;
    // A drawn card whose message went with its slot (TeamSpeak's line limit dropped the oldest lines, a clear):
    // its picture goes too (nothing else would free it while the chat lives).
    if (!view.rendered.isEmpty()) {
        QSet<QString> live;
        for (const SlotRef& ref : qAsConst(view.refs))
            live.insert(ref.slot.name);
        for (auto r = view.rendered.begin(); r != view.rendered.end();) {
            if (live.contains(r.key())) {
                ++r;
                continue;
            }
            view.document->addResource(QTextDocument::ImageResource, QUrl(r.key()), blankPicture());
            r = view.rendered.erase(r);
        }
    }
    return view.refs;
}

bool ChatLinkPreviews::refOf(QTextBrowser* browser, const QString& object, SlotRef* ref) const
{
    for (const SlotRef& r : slotsOf(browser)) {
        if (r.slot.name == object) {
            *ref = r;
            return true;
        }
    }
    return false;
}

bool ChatLinkPreviews::refOfSlot(QTextBrowser* browser, const QString& slotId, SlotRef* ref) const
{
    for (const SlotRef& r : slotsOf(browser)) {
        if (r.slotId == slotId) {
            *ref = r;
            return true;
        }
    }
    const auto it = m_views.find(browser);
    if (it == m_views.end())
        return false;
    for (const SlotRef& r : it->quiet) {
        if (r.slotId == slotId) {
            *ref = r;
            return true;
        }
    }
    return false;
}

QRectF ChatLinkPreviews::quietRect(QTextBrowser* browser, const SlotRef& ref) const
{
    // Where the quiet slot's link is, in viewport coordinates (the line holding the link's end).
    const QRectF line = linkdoc::lineRect(browser->document(), qMax(ref.linkStart, ref.linkEnd - 1));
    if (line.isEmpty())
        return {};
    return QRectF(line.left() - browser->horizontalScrollBar()->value(), line.top() - browser->verticalScrollBar()->value(), browser->viewport()->width(),
                  line.height());
}

int ChatLinkPreviews::slotCount(QTextBrowser* browser) const
{
    return slotsOf(browser).size();
}

QString ChatLinkPreviews::describe(QTextBrowser* browser, const QString& find) const
{
    QStringList lines;
    auto*       self = const_cast<ChatLinkPreviews*>(this);
    QVector<SlotRef> all = slotsOf(browser);
    if (const auto v = m_views.find(browser); v != m_views.end())
        all += v->quiet; // 2.5: quiet slots print as "absent"
    std::sort(all.begin(), all.end(), [](const SlotRef& a, const SlotRef& b) { return a.block != b.block ? a.block < b.block : a.linkEnd < b.linkEnd; });
    const QRectF shown = QRectF(browser->viewport()->rect()).adjusted(0, -kOnScreenMargin, 0, kOnScreenMargin);
    for (const SlotRef& r : qAsConst(all)) {
        if (!find.isEmpty() && !browser->document()->findBlockByNumber(r.block).text().contains(find))
            continue;
        const linkstate::Decision   d = self->decisionFor(r, false);
        const linkstate::LocalState l = self->slotLocal(r, m_local.value(r.target.key));
        const qint64                size = d.look == linkcard::Look::Click ? shownSize(r, l) : d.look == linkcard::Look::TooLarge ? l.size : -1;
        // 2.5: why it shows nothing or waits.
        const bool   onScreen = r.quiet() ? quietRect(browser, r).intersects(shown) : objectRect(browser, r.slot.object, r.slot.size).intersects(shown);
        const QString sender  = senderKey(contextOf(browser), r.senderUid);
        const linkstate::HoldReason hold = linkstate::holdReason(d, l, contextFor(r), onScreen, freeTurn(sender),
                                                                 m_budget.has(sender, wallMs(), r.target.expectedRequests), gateOf(r.target),
                                                                 linkpreview::autoload::autoCap(r.target, Settings::instance(), contextOf(browser).serverUid) > 0);
        lines << QString::fromLatin1("block %1 %2 %3 %4 server=%5 message=%6 slot=%7x%8")
                         .arg(r.block)
                         .arg(kindText(r.target.kind), r.target.host, d.media ? QString::fromLatin1("media") : lookName(d.look))
                         .arg(d.fromServer ? 1 : 0)
                         .arg(r.messageKey.isEmpty() ? QString::fromLatin1("-") : r.messageKey)
                         .arg(qRound(r.slot.size.width()))
                         .arg(qRound(r.slot.size.height()))
                     + QString::fromLatin1(" wait=%1 size=%2").arg(waitName(d.wait), size > 0 ? QString::number(size) : QString::fromLatin1("-"))
                     + (d.media ? QString::fromLatin1(" key=") + d.mediaKey.left(16) : QString())
                     + QString::fromLatin1(" cls=%1 prov=%2 kind=%3 hold=%4")
                           .arg(QString::fromLatin1(linkpreview::className(r.target.cls)), r.target.provider.isEmpty() ? QString::fromLatin1("-") : r.target.provider,
                                kindText(l.kind != linkpreview::Kind::None ? l.kind : r.target.kind), QString::fromLatin1(linkstate::holdReasonName(hold)));
    }
    return lines.join(QLatin1Char('\n'));
}

bool ChatLinkPreviews::ownsKey(const QString& key) const
{
    // Files from websites, and the files of senders' announcements (never ordinary uploads).
    return !key.isEmpty() && (m_core->isLocalFile(key) || m_serverKeys.contains(key));
}

// ---- drawing ------------------------------------------------------------------------------------------------

// 2.5 zoom: ChatScale's pass (inside its edit block): every card slot of the chat gets its du size for the new scale
// (the column may hold fewer du now) as du x s; no drawing. Media slots are ChatIntegration's objects (it sized them).
// Every card is drawn again at the new ratio right before the chat paints (renderVisible).
void ChatLinkPreviews::applyScale(QTextBrowser* browser, const PreviewScale& scale)
{
    View* view = viewOf(browser);
    if (!view || !view->document || view->document != browser->document() || !enabled())
        return;
    const PreviewStyle     style = styleOf(browser);
    const QVector<SlotRef> refs  = slotsOf(browser);
    QTextDocument*         doc   = view->document;
    const bool             wasMutating = m_chat->m_mutating;
    m_chat->m_mutating                 = true;
    for (const SlotRef& ref : refs) {
        if (!ref.slot.isCard())
            continue;
        const linkstate::Decision d = decisionFor(ref, false);
        linkstate::Decision       c = d;
        c.media                     = false;
        const QSizeF want           = QSizeF(cardSize(cardFor(ref, c, style), style));
        linkdoc::resizeObject(doc, ref.slot.object, want.isEmpty() ? ref.slot.size : want, scale);
    }
    m_chat->m_mutating = wasMutating;
    view->rendered.clear();
    view->refsValid = false;
    if (m_yt) {
        m_yt->forgetShownNotes(); // drawn from scratch: notes that aren't remembered go
        m_yt->relayout();         // the anchored player re-placed (3.3)
    }
}

void ChatLinkPreviews::pictureSwapped(QTextBrowser* browser, const QString& name)
{
    if (View* view = viewOf(browser))
        view->rendered.remove(name); // drawn again when it is within 400 px of the view
}

PreviewStyle ChatLinkPreviews::styleOf(QTextBrowser* browser) const
{
    PreviewStyle style = m_chat->styleFor(browser);
    // The layout's content column (Cozy / Compact indent) within the chat's margins; in a bubble its right
    // margin too, like the poll cards. 2.5 zoom: in du (the margins measured in the chat converted at its scale).
    const int indent = m_chat->m_layout ? m_chat->m_layout->indentFor(browser) : 0;
    const int bubble = m_chat->m_layout ? m_chat->m_layout->bubbleRightFor(browser) : 0;
    style.maxWidth   = previewscale::cardMaxWidthDu(style.maxWidth, m_chat->previewAreaWidth(browser), indent, browser->document()->documentMargin(), bubble,
                                                    m_chat->scaleOf(browser));
    style.hovered    = false;
    style.pressed    = false;
    return style;
}

QSize ChatLinkPreviews::cardSize(const linkcard::Card& card, const PreviewStyle& style) const
{
    const QSize s = linkcard::layout(card, style).size;
    return QSize(s.width(), s.height() + kGapTop + kGapBottom);
}

linkcard::Pointer ChatLinkPreviews::pointerFor(QTextBrowser* browser, const QString& object) const
{
    linkcard::Pointer p;
    if (m_hover.browser == browser && m_hover.object == object) {
        p.over  = true;
        p.hover = m_hover.zone;
    }
    if (m_press.browser == browser && m_press.object == object)
        p.pressed = m_press.zone;
    return p;
}

QString ChatLinkPreviews::signatureOf(const linkcard::Card& card, const PreviewStyle& style, const linkcard::Pointer& pointer) const
{
    // Everything the card is drawn from (2.5 too: a "Site busy" countdown as shown, the card's picture, its cover, its
    // text), and the ring's turn.
    QString signature = linkcard::signature(card, style, pointer);
    if (spinning(card) && style.animate)
        signature += QLatin1Char('|') + QString::number(QDateTime::currentMSecsSinceEpoch() / kSpinMs); // the ring turns
    return signature;
}

// size: the slot's du size (gaps included); the rect: its content in viewport px (2.5 zoom: du x s).
QRectF ChatLinkPreviews::objectRect(QTextBrowser* browser, int position, const QSizeF& duSize) const
{
    const PreviewScale scale = m_chat->scaleOf(browser);
    const QSizeF       size  = scale.toLogical(duSize);
    QTextDocument*   doc   = browser->document();
    const QTextBlock block = doc->findBlock(position);
    if (!block.isValid() || !block.layout())
        return {};
    const int       rel  = position - block.position();
    const QTextLine line = block.layout()->lineForTextPosition(rel);
    if (!line.isValid())
        return {};
    const QRectF blockRect = doc->documentLayout()->blockBoundingRect(block);
    const qreal  x         = line.cursorToX(rel);
    const qreal  top       = qMax(line.y(), line.y() + line.ascent() - size.height()) + kGapTop * scale.s();
    return QRectF(blockRect.left() + x - browser->horizontalScrollBar()->value(), blockRect.top() + top - browser->verticalScrollBar()->value(), size.width(),
                  (duSize.height() - kGapTop - kGapBottom) * scale.s());
}

void ChatLinkPreviews::renderCard(QTextBrowser* browser, View& view, const SlotRef& ref)
{
    const PreviewStyle        style = styleOf(browser);
    const linkstate::Decision d     = decisionFor(ref, false);
    if (d.media && m_core->entry(d.mediaKey))
        return; // the document will get the media (a reconcile is due)
    // 2.5: an LPX's media Core doesn't know yet (asked for once on screen, paced): its strip until then.
    linkcard::Card card = cardFor(ref, d, style);
    if (card.look == linkcard::Look::Absent)
        return; // nothing is drawn (a reconcile takes the object out)
    if (card.look == linkcard::Look::YouTube || card.look == linkcard::Look::Poster) {
        const linkcard::Layout l = linkcard::layout(card, style);
        card.thumbnail           = thumbnailFor(ref, d, (l.thumb.size() * style.dpr).toSize());
        if (card.thumbnail.isNull() && d.fromServer) {
            const linkstate::ServerState s = serverOf(ref.messageKey, ref.target, false);
            const MediaEntry*            e = m_core->entry(s.mainKey);
            card.thumbnailLoading = e && (e->state == MediaState::Queued || e->state == MediaState::Downloading || (e->state == MediaState::Ready));
        }
    }
    if (card.look == linkcard::Look::Embed && card.data.layout != linkpreview::CardLayout::None) {
        // 2.5: the card's picture (ours from cache/links, or the sender's from the TeamSpeak server through Core).
        const linkcard::Layout l = linkcard::layout(card, style);
        card.picture             = thumbnailFor(ref, d, (l.thumb.size() * style.dpr).toSize());
        if (!card.picture.isNull())
            card.pictureLoading = false;
        else if (d.fromServer)
            queueLpxPicture(ref, serverOf(ref.messageKey, ref.target, false).mainKey); // drawn on screen: its picture is wanted
    }
    const linkcard::Pointer pointer   = pointerFor(browser, ref.slot.name);
    const QString           signature = signatureOf(card, style, pointer);
    if (view.rendered.value(ref.slot.name) == signature)
        return;
    const QImage picture = linkcard::render(card, style, pointer);
    const qreal  dpr     = picture.devicePixelRatio() > 0 ? picture.devicePixelRatio() : 1.0; // 2.5 zoom: R (may be below 1)
    QImage       padded(QSize(picture.width(), picture.height() + qRound((kGapTop + kGapBottom) * dpr)), QImage::Format_ARGB32_Premultiplied);
    padded.setDevicePixelRatio(dpr);
    padded.fill(Qt::transparent);
    {
        QPainter p(&padded);
        p.drawImage(QPointF(0, kGapTop), picture);
    }
    if (ChatScale* scaler = m_chat->chatScale(browser)) // 2.5 zoom: booked for the chat's picture budget
        scaler->putPicture(ref.slot.name, QPixmap::fromImage(previewscale::forDocument(padded, m_chat->scaleOf(browser).displayRatio())), ChatScale::Links);
    else
        view.document->addResource(QTextDocument::ImageResource, QUrl(ref.slot.name), QPixmap::fromImage(previewscale::forDocument(padded, m_chat->scaleOf(browser).displayRatio())));
    view.rendered.insert(ref.slot.name, signature);
    if (spinning(card) && style.animate) {
        const QRectF r = objectRect(browser, ref.slot.object, ref.slot.size);
        view.spinRects.append(r.toAlignedRect().adjusted(-2, -2, 2, 2));
    }
}

void ChatLinkPreviews::renderVisible(QTextBrowser* browser)
{
    View* view = viewOf(browser);
    if (!view || !view->document || view->document != browser->document())
        return;
    const QVector<SlotRef> refs = slotsOf(browser);
    view->spinRects.clear();
    if (refs.isEmpty() && view->quiet.isEmpty())
        return;
    const QRectF shown = QRectF(browser->viewport()->rect()).adjusted(0, -kOnScreenMargin, 0, kOnScreenMargin);
    const QRectF far   = QRectF(browser->viewport()->rect()).adjusted(0, -3000, 0, 3000);
    const PreviewStyle style = styleOf(browser);
    bool         resize = false;
    // 2.4 auto-load: this chat's on-screen candidates replace its last ones (what scrolled away drops out).
    for (int i = m_autoWanted.size() - 1; i >= 0; --i) {
        if (!m_autoWanted.at(i).browser || m_autoWanted.at(i).browser == browser)
            m_autoWanted.removeAt(i);
    }
    QVector<AutoWant> wanted;
    const bool        autoOn = enabled() && !m_stopped && Settings::instance().autoLoadLinkPreviews;
    // 2.5 (3.12): why on-screen slots don't load, counted for the diagnostic info (the last pass's).
    m_holdCounts.clear();
    const ChatContext holdCtx = contextOf(browser);
    const auto        noteHold = [&](const SlotRef& ref, const linkstate::Decision& d) {
        if (d.media || d.fromServer || (d.look != linkcard::Look::Absent && d.look != linkcard::Look::Click && d.look != linkcard::Look::Loading))
            return;
        const QString               sender = senderKey(holdCtx, ref.senderUid);
        const linkstate::LocalState l      = slotLocal(ref, m_local.value(ref.target.key));
        const linkstate::HoldReason hold   = linkstate::holdReason(d, l, contextFor(ref), true, freeTurn(sender),
                                                                 m_budget.has(sender, wallMs(), ref.target.expectedRequests), gateOf(ref.target),
                                                                 linkpreview::autoload::autoCap(ref.target, Settings::instance(), holdCtx.serverUid) > 0);
        if (hold != linkstate::HoldReason::None)
            m_holdCounts[QString::fromLatin1(linkstate::holdReasonName(hold))] += 1;
    };
    for (const SlotRef& ref : refs) {
        if (!ref.slot.isCard())
            continue;
        const QRectF rect = objectRect(browser, ref.slot.object, ref.slot.size);
        if (rect.intersects(shown)) {
            const linkstate::Decision d = decisionFor(ref, false);
            if (d.media && !m_core->entry(d.mediaKey) && d.fromServer) {
                // 2.5: an LPX's media on screen: asked of Core, paced (one a second per connection, within the flood
                // model's reserve). Its strip stays until Core knows the file.
                queueLpxPicture(ref, d.mediaKey);
            } else if (d.media || cardSize(cardFor(ref, d, style), style) != ref.slot.size.toSize()) { // 2.5 zoom: du against du
                resize = true; // laid out again first (not while the chat paints)
                continue;
            }
            noteHold(ref, d);
            if (d.wait == linkstate::Wait::Auto && browser->isVisible()) {
                wanted.append({QPointer<QTextBrowser>(browser), ref.slot.name, ref.slotId});
            } else if (d.wait == linkstate::Wait::Own || d.wait == linkstate::Wait::Sender || d.wait == linkstate::Wait::Checking
                       || d.wait == linkstate::Wait::Host) {
                m_waitSeen = true; // looked at again in a second (its time, the sender, the busy site)
            } else if (autoOn && browser->isVisible() && autoCandidate(ref, d)) {
                // Held back, or no budget token yet: runAutoLoads starts it in its turn; looked at again in a second.
                wanted.append({QPointer<QTextBrowser>(browser), ref.slot.name, ref.slotId});
                m_waitSeen = true;
            }
            renderCard(browser, *view, ref);
        } else if (view->rendered.contains(ref.slot.name) && !rect.intersects(far)) {
            // Far off screen: its memory goes (drawn again when it comes back).
            view->document->addResource(QTextDocument::ImageResource, QUrl(ref.slot.name), blankPicture());
            if (ChatScale* scaler = m_chat->chatScale(browser))
                scaler->forget(ref.slot.name);
            view->rendered.remove(ref.slot.name);
        }
    }
    // 2.5: quiet slots (no object: the web class while nothing is shown, a sender's "nothing", ...) load lazily too, by
    // the place of their link: the same three branches as a drawn slot, read from the look behind Absent.
    for (const SlotRef& ref : qAsConst(view->quiet)) {
        if (!quietRect(browser, ref).intersects(shown))
            continue;
        const linkstate::Decision d = decisionFor(ref, false);
        if (d.look != linkcard::Look::Absent || d.media) {
            resize = true; // it has something to show now: the next reconcile puts it in
            continue;
        }
        noteHold(ref, d);
        if (d.underlying == linkcard::Look::Loading && d.wait == linkstate::Wait::Auto && browser->isVisible()) {
            wanted.append({QPointer<QTextBrowser>(browser), QString(), ref.slotId});
        } else if (d.wait == linkstate::Wait::Own || d.wait == linkstate::Wait::Sender || d.wait == linkstate::Wait::Checking
                   || d.wait == linkstate::Wait::Host) {
            m_waitSeen = true;
        } else if (autoOn && browser->isVisible() && autoCandidate(ref, d)) {
            wanted.append({QPointer<QTextBrowser>(browser), QString(), ref.slotId});
            m_waitSeen = true;
        }
    }
    if (resize)
        scheduleRefresh();
    if (!view->spinRects.isEmpty() && !m_spinTimer->isActive())
        m_spinTimer->start();
    if (!wanted.isEmpty()) {
        // The newest (bottom) first: what was just posted is what the reader looks at.
        for (int i = wanted.size() - 1; i >= 0; --i)
            m_autoWanted.append(wanted.at(i));
        if (!m_autoTimer->isActive())
            m_autoTimer->start();
    }
    if (m_waitSeen && !m_waitTimer->isActive() && !m_stopped)
        m_waitTimer->start();
}

void ChatLinkPreviews::repaintCard(QTextBrowser* browser, const QString& object)
{
    View*   view = viewOf(browser);
    SlotRef ref;
    if (!view || !refOf(browser, object, &ref))
        return;
    view->rendered.remove(object);
    renderCard(browser, *view, ref);
    browser->viewport()->update(objectRect(browser, ref.slot.object, ref.slot.size).toAlignedRect().adjusted(-2, -6, 2, 6));
}

// ---- the pointer ---------------------------------------------------------------------------------------------------

ChatLinkPreviews::Spot ChatLinkPreviews::spotAt(QTextBrowser* browser, const QPoint& pos, QRectF* rectOut, SlotRef* refOut) const
{
    Spot spot;
    for (const SlotRef& ref : slotsOf(browser)) {
        if (!ref.slot.isCard())
            continue;
        const QRectF rect = objectRect(browser, ref.slot.object, ref.slot.size);
        if (!rect.contains(pos))
            continue;
        auto*                     self = const_cast<ChatLinkPreviews*>(this);
        const PreviewStyle        style = styleOf(browser);
        const linkstate::Decision d     = self->decisionFor(ref, false);
        const linkcard::Card      card  = self->cardFor(ref, d, style);
        const linkcard::Layout    l     = linkcard::layout(card, style);
        spot.browser                    = browser;
        spot.object                     = ref.slot.name;
        spot.zone                       = linkcard::zoneAt(card, l, m_chat->scaleOf(browser).toDu(QPointF(pos) - rect.topLeft()), true); // 2.5 zoom: du
        if (rectOut)
            *rectOut = rect;
        if (refOut)
            *refOut = ref;
        return spot;
    }
    return spot;
}

void ChatLinkPreviews::setHover(const Spot& spot)
{
    if (spot.same(m_hover))
        return;
    const Spot old = m_hover;
    m_hover        = spot;
    if (old.browser && !old.object.isEmpty())
        repaintCard(old.browser, old.object);
    if (spot.browser && !spot.object.isEmpty() && (spot.object != old.object || spot.browser != old.browser || spot.zone != old.zone))
        repaintCard(spot.browser, spot.object);
    if (spot.object.isEmpty() && old.browser)
        applyCursor(old.browser, spot);
}

void ChatLinkPreviews::applyCursor(QTextBrowser* browser, const Spot& spot)
{
    if (!browser)
        return;
    QWidget* vp = browser->viewport();
    if (!spot.object.isEmpty()) {
        if (!m_cursorSet) {
            m_savedCursor = vp->cursor().shape() == Qt::PointingHandCursor ? Qt::ArrowCursor : vp->cursor().shape();
            m_cursorSet   = true;
        }
        SlotRef ref;
        bool    actionable = spot.zone != linkcard::Zone::None && spot.zone != linkcard::Zone::Body;
        if (spot.zone == linkcard::Zone::Body && refOf(browser, spot.object, &ref)) {
            const linkstate::Decision d = decisionFor(ref, false);
            actionable                  = linkcard::isActionable(cardFor(ref, d, styleOf(browser)), spot.zone);
        }
        const Qt::CursorShape shape = actionable ? Qt::PointingHandCursor : Qt::ArrowCursor;
        if (vp->cursor().shape() != shape)
            vp->setCursor(shape);
    } else if (m_cursorSet) {
        vp->setCursor(m_savedCursor);
        m_cursorSet = false;
    }
}

bool ChatLinkPreviews::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::Paint && type != QEvent::MouseMove && type != QEvent::Leave && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
        && type != QEvent::MouseButtonDblClick && type != QEvent::ToolTip && type != QEvent::Resize && type != QEvent::ContextMenu)
        return false;
    QTextBrowser* browser = nullptr;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && it->browser->viewport() == watched)
            browser = it->browser;
    }
    if (!browser)
        return false;
    switch (type) {
    case QEvent::Paint:
        renderVisible(browser);
        return false;
    case QEvent::Resize:
        scheduleRefresh(); // another width
        return false;
    case QEvent::Leave:
        setHover(Spot());
        return false;
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->buttons() & Qt::LeftButton)
            return m_press.browser == browser && !m_press.object.isEmpty(); // a press on a card: no text selection from there
        const Spot spot     = spotAt(browser, me->pos());
        const bool entering = !spot.object.isEmpty() && m_hover.object.isEmpty();
        // Onto a card: the chat's own hover (a preview's look, video controls) ends, as it would on a move it saw.
        if (entering && m_chat->m_media)
            m_chat->updateHoverAt(browser, QPointF(-100000, -100000), true);
        setHover(spot);
        applyCursor(browser, spot);
        return !spot.object.isEmpty(); // our cursor, not QTextBrowser's
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto*      me   = static_cast<QMouseEvent*>(event);
        const Spot spot = spotAt(browser, me->pos());
        if (spot.object.isEmpty())
            return false;
        if (me->button() == Qt::RightButton)
            return true; // the context menu event follows
        if (me->button() != Qt::LeftButton)
            return false;
        if (type == QEvent::MouseButtonDblClick)
            return true; // the first click did it
        m_press = spot;
        repaintCard(browser, spot.object);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::RightButton)
            return !spotAt(browser, me->pos()).object.isEmpty();
        if (me->button() != Qt::LeftButton || m_press.object.isEmpty())
            return false;
        const Spot pressed = m_press;
        m_press            = Spot();
        SlotRef    ref;
        const Spot spot = spotAt(browser, me->pos(), nullptr, &ref);
        if (pressed.browser == browser)
            repaintCard(browser, pressed.object);
        if (spot.same(pressed))
            activate(browser, ref, spot.zone);
        return true;
    }
    case QEvent::ContextMenu: {
        auto*      ce = static_cast<QContextMenuEvent*>(event);
        SlotRef    ref;
        const Spot spot = spotAt(browser, ce->pos(), nullptr, &ref);
        if (spot.object.isEmpty())
            return false;
        showCardMenu(browser, ref, ce->globalPos());
        return true;
    }
    case QEvent::ToolTip: {
        auto*      help = static_cast<QHelpEvent*>(event);
        QRectF     rect;
        SlotRef    ref;
        const Spot spot = spotAt(browser, help->pos(), &rect, &ref);
        if (spot.object.isEmpty())
            return false;
        const linkstate::Decision d   = decisionFor(ref, false);
        const QString             tip = linkcard::toolTipHtml(cardFor(ref, d, styleOf(browser)), spot.zone); // escaped: titles are other people's text
        if (tip.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), tip, browser->viewport(), rect.toAlignedRect());
        return true;
    }
    default:
        return false;
    }
}

void ChatLinkPreviews::activate(QTextBrowser* browser, const SlotRef& ref, linkcard::Zone zone)
{
    Q_UNUSED(browser);
    switch (zone) {
    case linkcard::Zone::Load:
    case linkcard::Zone::Retry:
        startFetch(ref.target);
        break;
    case linkcard::Zone::Open:
        if (m_yt && ref.target.kind == linkpreview::Kind::YouTube) // 2.5: with the time a player stopped at
            openInBrowser(m_yt->watchUrlOf(ref.slotId, ref.target.videoId, linkstate::webAddress(ref.slot.href, ref.target)));
        else
            openInBrowser(linkstate::webAddress(ref.slot.href, ref.target)); // the link as written (a start time stays)
        break;
    case linkcard::Zone::Play: // 2.5 YouTube player (Play, Resume, the popup to the front)
        if (m_yt)
            m_yt->play(ref.slotId, ref.target.videoId, linkstate::webAddress(ref.slot.href, ref.target));
        break;
    case linkcard::Zone::Stop:
        if (m_yt)
            m_yt->stop(ref.slotId);
        break;
    case linkcard::Zone::ConsentPlay:
        if (m_yt)
            m_yt->consentPlay(ref.slotId);
        break;
    case linkcard::Zone::ConsentBrowser:
        if (m_yt)
            m_yt->consentBrowser(ref.slotId);
        break;
    case linkcard::Zone::Terms:
        openInBrowser(ytpage::termsUrl());
        break;
    case linkcard::Zone::Privacy:
        openInBrowser(ytpage::privacyUrl());
        break;
    case linkcard::Zone::Hide:
        setHidden(ref.slotId, ref.target.key, true);
        break;
    case linkcard::Zone::Show:
        setHidden(ref.slotId, ref.target.key, false);
        break;
    case linkcard::Zone::Reveal:
        // 2.5: a sensitive card's picture, shown for this slot for the session.
        m_revealed.insert(ref.slotId);
        if (browser)
            repaintCard(browser, ref.slot.name);
        break;
    case linkcard::Zone::Body:
    case linkcard::Zone::None:
        break;
    }
}

void ChatLinkPreviews::showCardMenu(QTextBrowser* browser, const SlotRef& ref, const QPoint& globalPos)
{
    const linkstate::Decision d    = decisionFor(ref, false);
    const linkcard::Card      card = cardFor(ref, d, styleOf(browser));
    auto*                     menu = new QMenu(browser);
    menu->setObjectName(QString::fromLatin1("tsmediaLinkMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    const QPointer<ChatLinkPreviews> self(this);
    const linkpreview::Target        target = ref.target;
    const QString                    slotId = ref.slotId;
    QAction*                         primary = nullptr;
    // The bold item is what a click on the card does (linkcard::menuDefault): "No preview for this page" opens the
    // browser, so it has no Retry.
    switch (linkcard::menuDefault(card)) {
    case linkcard::Zone::Load:
        primary = menu->addAction(i18n::t("&Load preview"), this, [self, target] {
            if (self)
                self->startFetch(target);
        });
        break;
    case linkcard::Zone::Retry:
        primary = menu->addAction(i18n::t("Re&try"), this, [self, target] { // R is Reply's (below)
            if (self)
                self->startFetch(target);
        });
        break;
    case linkcard::Zone::Show:
        primary = menu->addAction(i18n::t("&Show preview"), this, [self, slotId, target] {
            if (self)
                self->setHidden(slotId, target.key, false);
        });
        break;
    default:
        break;
    }
    const QUrl web  = linkstate::webAddress(ref.slot.href, target); // the link as written (a start time stays)
    QAction*   open = menu->addAction(i18n::t("Open in &browser"), this, [self, web] {
        if (self)
            self->openInBrowser(web);
    });
    if (!primary)
        primary = open;
    if (card.look == linkcard::Look::YouTube && m_yt) {
        // 2.5 YouTube player: in a window (any card), again after a note, stop, and YouTube's Terms (YouTube's RMF).
        const QString videoId = target.videoId;
        if (card.player == linkcard::Player::Note) {
            menu->addAction(i18n::t("Try a&gain"), this, [self, slotId, videoId, web] {
                if (self && self->m_yt)
                    self->m_yt->tryAgain(slotId, videoId, web);
            });
        } else if (linkcard::playerOpen(card)) {
            menu->addAction(i18n::t("&Stop video"), this, [self, slotId] {
                if (self && self->m_yt)
                    self->m_yt->stop(slotId);
            });
        } else if (card.playable && card.player != linkcard::Player::Consent) {
            // Only while the player over the card exists (the release gate): in 2.5.0 Play itself is the window.
            if constexpr (ytpage::kAnchoredPlayer) {
                menu->addAction(i18n::t("Play in a &window"), this, [self, slotId, videoId, web] {
                    if (self && self->m_yt)
                        self->m_yt->play(slotId, videoId, web, true);
                });
            }
        }
        if (Settings::instance().playYouTubeInChat) {
            menu->addAction(ytpage::termsText(), this, [self] {
                if (self)
                    self->openInBrowser(ytpage::termsUrl());
            });
        }
    }
    // Its own access key: L is Load preview's.
    menu->addAction(i18n::t("&Copy link"), this, [self, web, browser = QPointer<QTextBrowser>(browser)] {
        QGuiApplication::clipboard()->setText(web.toString(QUrl::FullyEncoded));
        if (self && browser)
            self->m_chat->showFeedback(browser->viewport(), i18n::t("Link copied"), false);
    });
    if (card.look != linkcard::Look::Hidden) {
        menu->addSeparator();
        menu->addAction(i18n::t("&Hide preview"), this, [self, slotId, target] {
            if (self)
                self->setHidden(slotId, target.key, true);
        });
    }
    // The message's own items: Add reaction (text reactions), Reply; 2.5: Load link previews when it applies.
    if (m_chat->m_textReactions) {
        if (QAction* react = m_chat->m_textReactions->menuAction(menu, browser, ref.block)) {
            menu->addSeparator();
            menu->addAction(react);
        }
    }
    if (QAction* load = menuAction(menu, browser, ref.block))
        menu->addAction(load);
    if (m_chat->m_replies)
        m_chat->m_replies->addPreviewMenu(menu, browser);
    menu->setDefaultAction(primary);
    menu->popup(globalPos);
}

bool ChatLinkPreviews::addMediaMenu(QMenu* menu, QTextBrowser* browser, const QString& key, const QPoint& viewportPos, QUrl* webUrl,
                                    linkpreview::Target* targetOut)
{
    if (!menu || !browser || key.isEmpty())
        return false;
    const QString name = linkdoc::mediaPrefix() + key;
    // The slot under the pointer (the same media can be in several messages).
    const QPointF docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int     hit   = browser->document()->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    const int     block = hit >= 0 ? browser->document()->findBlock(hit).blockNumber() : -1;
    SlotRef       found;
    bool          any = false;
    for (const SlotRef& r : slotsOf(browser)) {
        if (r.slot.name != name)
            continue;
        if (!any || r.block == block) {
            found = r;
            any   = true;
        }
    }
    if (!any)
        return false;
    const QUrl web = linkstate::webAddress(found.slot.href, found.target); // the link as written
    if (webUrl)
        *webUrl = web;
    if (targetOut)
        *targetOut = found.target; // 2.5 GIFs
    const QPointer<ChatLinkPreviews> self(this);
    const linkpreview::Target        target = found.target;
    const QString                    slotId = found.slotId;
    menu->addSeparator();
    menu->addAction(i18n::t("Open in &browser"), this, [self, web] {
        if (self)
            self->openInBrowser(web);
    });
    menu->addAction(i18n::t("&Hide preview"), this, [self, slotId, target] {
        if (self)
            self->setHidden(slotId, target.key, true);
    });
    return true;
}

// ---- actions ---------------------------------------------------------------------------------------------------

void ChatLinkPreviews::openInBrowser(const QUrl& url) const
{
    // Only the checked https address of a link (never anything a website or a chat made up beyond it).
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return;
    ts3::log(LogLevel_INFO, 0, "Link preview: opening %1 in the browser", {ts3::name(url.host())});
    QDesktopServices::openUrl(url);
}

void ChatLinkPreviews::setHidden(const QString& slotId, const QString& urlKey, bool hidden)
{
    if (hidden) {
        m_hidden.insert(slotId);
        // Its fetch goes when no other card of that link waits for it.
        if (m_local.value(urlKey).state == linkstate::Local::Loading) {
            bool wanted = false;
            for (auto it = m_views.constBegin(); it != m_views.constEnd() && !wanted; ++it) {
                for (const SlotRef& r : slotsOf(it.key()))
                    wanted = wanted || (r.slot.urlKey == urlKey && !m_hidden.contains(r.slotId));
            }
            if (!wanted)
                cancelFetch(urlKey);
        }
    } else {
        m_hidden.remove(slotId);
    }
    scheduleRefresh();
    updateVisibleViewports();
}

void ChatLinkPreviews::startFetch(const linkpreview::Target& target, Origin origin, const QString& serverUid, const QString& sender)
{
    if (!enabled() || m_down || m_stopped || target.key.isEmpty() || target.kind == linkpreview::Kind::None || target.cls == linkpreview::LinkClass::None)
        return;
    if (target.cls == linkpreview::LinkClass::Web && !Settings::instance().showWebCards)
        return; // never a class the user switched off
    if (m_local.value(target.key).state == linkstate::Local::Loading) {
        // R26: a click on a link that loads by itself: that load is kept, now as the user's (its failure shows). It
        // still runs with the automatic caps and its sender's turn until it ends: a file over them becomes a click
        // card with its size (a real click loads it then), never "Too large".
        if (origin == Origin::Click) {
            m_local[target.key].clicked = true;
            const auto run = m_autoRuns.find(target.key);
            if (run != m_autoRuns.end())
                run->clicked = true;
            updateVisibleViewports();
        }
        return; // one fetch per link
    }
    // 2.5: a busy site's rounds and how long it has cooled stay with the link (6.4).
    const linkstate::LocalState before = m_local.value(target.key);
    linkstate::LocalState       l;
    l.state       = linkstate::Local::Loading;
    l.clicked     = origin == Origin::Click;
    l.coolRetries = before.coolRetries;
    l.coolingMs   = before.coolingMs;
    l.webCards    = Settings::instance().showWebCards; // what its Caps hold (restartWebCardsLoads)
    m_local.insert(target.key, l);
    m_targets.insert(target.key, target);
    if (m_pipeline && m_pipeline->pending(target.key)) {
        // 2.4 auto-load: our own pipeline loads this very link (now, or once its turn comes): no second request, its
        // download is shown (its end, whatever it is, brings this slot back to the normal rules: applyOwnRecord).
        m_pipelineWaits.insert(target.key);
        ts3::log(LogLevel_INFO, 0, "Link preview: the %1 from %2 comes with your own download", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
        updateVisibleViewports();
        return;
    }
    const QString dir = linksDir();
    QDir().mkpath(dir);
    // 2.5 (D38): what this load may download: a click's caps, or the automatic ones of its chat's server.
    linkpreview::Caps caps = origin == Origin::Auto ? linkpreview::autoload::autoCaps(target, Settings::instance(), serverUid)
                                                    : linkpreview::autoload::clickCaps(target);
    caps.webCards         = l.webCards; // a click too: a share link that is another web page stays unloaded
    const qint64 maxBytes = target.kind == linkpreview::Kind::Card ? caps.picture : caps.media;
    if (origin == Origin::Auto) {
        AutoRun run;
        run.sender     = sender;
        run.serverUid  = serverUid;
        run.maxBytes   = maxBytes;
        run.mediaBytes = caps.media;
        run.videoBytes = caps.video;
        m_autoRuns.insert(target.key, run);
    }
    if (origin == Origin::Auto)
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 (automatic)", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    else
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 (clicked)", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    if (m_loader)
        m_loader(target, caps, dir);
    updateVisibleViewports();
}

void ChatLinkPreviews::cancelFetch(const QString& urlKey)
{
    if (m_local.value(urlKey).state != linkstate::Local::Loading)
        return;
    const bool wasAuto = m_autoRuns.remove(urlKey) > 0;
    if (!m_pipelineWaits.remove(urlKey) && m_fetcher)
        m_fetcher->cancel(urlKey); // our pipeline's download (a wait) is never canceled from here
    m_local.remove(urlKey);
    if (wasAuto)
        updateVisibleViewports(); // the next on-screen pass refills the queue
}

void ChatLinkPreviews::restartWebCardsLoads()
{
    // Review fix: a share link's load keeps the Caps::webCards it started with. Turned off, it would still load the
    // other website's page (and picture) it turned out to be; turned on, it would end without a preview (kWebOff) for
    // the session. It starts again now, as the user's or as the automatic load it was (its sender's turn kept, no
    // second budget token). Our pipeline's own download (a wait) is restarted by the pipeline (SenderPipeline).
    const bool  web = Settings::instance().showWebCards;
    QStringList keys;
    for (auto it = m_local.constBegin(); it != m_local.constEnd(); ++it) {
        if (!m_pipelineWaits.contains(it.key()) && linkstate::restartForWebCards(it.value(), m_targets.value(it.key()), web))
            keys << it.key();
    }
    for (const QString& key : qAsConst(keys)) {
        const linkstate::LocalState before = m_local.value(key);
        const linkpreview::Target   target = m_targets.value(key);
        const bool                  wasRun = m_autoRuns.contains(key);
        const AutoRun               run    = m_autoRuns.value(key);
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 again (cards for other web pages %3)",
                 {ts3::pub(kindText(target.kind)), ts3::name(target.host), ts3::pub(web ? QLatin1String("on") : QLatin1String("off"))});
        cancelFetch(key);
        linkstate::LocalState idle; // a busy site's rounds stay with the link (startFetch reads them)
        idle.coolRetries = before.coolRetries;
        idle.coolingMs   = before.coolingMs;
        m_local.insert(key, idle);
        if (wasRun) {
            startFetch(target, Origin::Auto, run.serverUid, run.sender);
            if (before.clicked)
                startFetch(target, Origin::Click); // R26: still the user's (with the automatic caps and turn it had)
        } else {
            startFetch(target, Origin::Click);
        }
        if (m_local.value(key).state == linkstate::Local::Idle)
            m_local.remove(key); // not started after all: the normal rules
    }
}

void ChatLinkPreviews::onProgress(const QString& key, qint64 received, qint64 total)
{
    auto it = m_local.find(key);
    if (it == m_local.end() || it->state != linkstate::Local::Loading)
        return;
    const auto run = m_autoRuns.find(key);
    if (run != m_autoRuns.end()) {
        run->received = received; // a lower limit stops it (settingsChanged)
        run->total    = total >= 0 ? total : -1;
    }
    const double p = total > 0 ? qBound(0.0, static_cast<double>(received) / static_cast<double>(total), 1.0) : -1.0;
    if (p >= 0 && it->progress >= 0 && p - it->progress < 0.01)
        return;
    it->progress = p;
    for (auto v = m_views.constBegin(); v != m_views.constEnd(); ++v) {
        if (v->browser && v->browser->isVisible())
            v->browser->viewport()->update();
    }
}

void ChatLinkPreviews::onFetched(const QString& key, const linkpreview::Result& result)
{
    applyFetched(key, result, false);
    // 2.5 GIFs: a result in cache/links (media, a card, a "none" record); a busy site or a cancel leaves nothing there.
    if (!m_down && (result.ok || linkstate::isNothingError(result.error) || result.error == QLatin1String(linkpreview::errors::kNotFound)
                    || result.error == QLatin1String(linkpreview::errors::kBadType)))
        emit linkLoaded(key);
}

void ChatLinkPreviews::applyFetched(const QString& key, const linkpreview::Result& result, bool fromCache, bool own)
{
    auto it = m_local.find(key);
    if (m_down || it == m_local.end() || it->state != linkstate::Local::Loading)
        return; // canceled meanwhile
    if (!result.ok && result.error == QLatin1String(linkpreview::errors::kCanceled))
        return; // the answer to a cancel of ours (a load of the link started again since): its own answer follows
    const AutoRun run     = m_autoRuns.value(key);
    const bool    wasAuto = m_autoRuns.remove(key) > 0;
    m_pipelineWaits.remove(key);
    m_cacheMiss.remove(key);
    const linkpreview::Target target = m_targets.value(key);
    if (wasAuto)
        updateVisibleViewports(); // a running slot is free: the next on-screen pass may start one
    if (!result.ok) {
        const bool    clicked = it->clicked;
        const QString error   = result.error;
        if (error == QLatin1String(linkpreview::errors::kRateLimited) || error == QLatin1String(linkpreview::errors::kGateBusy)) {
            // 2.5 (6.4): a busy site: an automatic load waits as long as the site asked and loads again by itself (at most
            // two hard rounds, ten minutes in all); a click gets the busy line.
            linkstate::LocalState next = linkstate::afterBusy(*it, result.retryAfterMs, result.softCooling,
                                                              error == QLatin1String(linkpreview::errors::kGateBusy), !clicked, wallMs());
            next.clicked = clicked;
            *it          = next;
            m_waitSeen   = true;
            if (!m_waitTimer->isActive() && !m_stopped)
                m_waitTimer->start();
        } else if (linkstate::isNothingError(error)) {
            // 2.5: the page has nothing to show (no card, a wall, an expired link): no slot (after a click: "No preview").
            // A share link that is another web page while cards for other web pages are off: the same, until they are on.
            it->state = linkstate::Local::Nothing;
            if (error == QLatin1String(linkpreview::errors::kWebOff)) {
                if (Settings::instance().showWebCards)
                    m_local.erase(it); // review fix: they went on meanwhile: the normal rules at once
                else
                    m_webOff.insert(key);
            }
        } else if (error == QLatin1String(linkpreview::errors::kTooLarge) && wasAuto
                   && !(result.size > linkpreview::autoload::mediaCapOf(linkpreview::autoload::clickCaps(target), result.kind))) {
            // 2.4 auto-load: above the automatic limit, nothing more of it is downloaded: a click card with its size
            // (or the limit, when the site doesn't say). Larger than a click may load: "Too large" below (no Load
            // preview that could only fail and ask the site once more). 2.5: the chat's server and the cap it hit, so a
            // raised limit makes it a candidate again (8.3); a web page that answered with media is a media slot now.
            it->state     = linkstate::Local::OverAutoLimit;
            it->size      = result.size;
            it->serverUid = run.serverUid;
            it->kind      = result.kind;
            // The cap that refused it: a page link that answered with media was refused by the media cap (8.3).
            linkpreview::Caps ran;
            ran.media     = run.mediaBytes;
            ran.video     = run.videoBytes;
            it->capHit    = linkpreview::autoload::isMediaKind(result.kind) ? linkpreview::autoload::mediaCapOf(ran, result.kind) : run.maxBytes;
        } else if (error == QLatin1String(linkpreview::errors::kTooLarge)) {
            it->state = linkstate::Local::TooLarge;
            it->size  = result.size; // what the website announced (-1: it didn't say)
            it->kind  = result.kind;
        } else {
            it->state = linkstate::Local::Failed;
            it->error = linkstate::errorText(error); // neutral: the reason is in the log only
        }
        ts3::log(LogLevel_INFO, 0, "Link preview: %1 from %2 not shown (%3)", {ts3::pub(kindText(target.kind)), ts3::name(target.host), ts3::pub(error)});
        scheduleRefresh();
        return;
    }
    if (!fromCache)
        pruneLinkCache(); // the cache of website files stays within its cap (the oldest go first, never this one)
    it = m_local.find(key);
    if (it == m_local.end())
        return;
    linkstate::LocalState& l = it.value();
    // 2.5: a page link is what its load turned out to be; the 2.4 classes keep their own kinds.
    const linkpreview::Kind kind = target.isNewClass() && result.kind != linkpreview::Kind::None ? result.kind : target.kind;
    l.kind                       = kind;
    switch (kind) {
    case linkpreview::Kind::YouTube:
        l.state         = linkstate::Local::Loaded;
        l.title         = linkcard::cleanText(result.title, 200);
        l.author        = linkcard::cleanText(result.author, 100);
        l.thumbnailFile = result.thumbnailFile;
        break;
    case linkpreview::Kind::Card:
        // 2.5: a site or web card (its picture, when it has one, in cache/links). A page on another website than the
        // link (a redirect): only for a load the user started or our own message; an automatic (or cached) one shows
        // nothing, like a 2.5 sender's LPXNO r=x (Load link previews shows it), and our own message's slot reads it
        // again from the cache (showHeldCard).
        l = linkstate::afterCard(l, result.card, result.finalHost, own);
        break;
    case linkpreview::Kind::Image:
    case linkpreview::Kind::Gif: {
        int w = result.width;
        int h = result.height;
        if (w <= 0 || h <= 0) {
            const std::unique_ptr<QImageReader> safe = safeimage::reader(result.localFile, safeimage::Source::Website); // 2.5: by its bytes only
            const QSize                         s    = safe->size(); // the header only
            w                                        = s.width();
            h                                        = s.height();
        }
        const MediaLink link = linkstate::webLink(target, result.mime, QFileInfo(result.localFile).size(), w, h, 0, false, result.card.sensitive);
        const QString   mk   = m_core->adoptLocalFile(link, result.localFile);
        if (mk.isEmpty()) {
            l.state = linkstate::Local::Failed;
            l.error = i18n::t("Couldn't show this file");
        } else {
            l.state    = linkstate::Local::Loaded;
            l.mediaKey = mk;
        }
        break;
    }
    case linkpreview::Kind::Video:
    case linkpreview::Kind::Loop:
        adoptVideo(target, result, kind == linkpreview::Kind::Loop);
        return; // its poster first (a worker of ours), then the slot
    case linkpreview::Kind::None:
        l.state = linkstate::Local::Failed;
        break;
    }
    scheduleRefresh();
}

// A video loaded from a website: its poster, size and length from mediaprobe (Media Foundation) on our
// worker, unless the fetcher made a poster; then it becomes a Core entry and plays like an uploaded one.
void ChatLinkPreviews::adoptVideo(const linkpreview::Target& target, const linkpreview::Result& result, bool loop)
{
    const QString file   = result.localFile;
    const QString mime   = result.mime;
    const QString poster = file + QString::fromLatin1(".poster.jpg");
    const QPointer<ChatLinkPreviews> self(this);
    const QString key       = target.key;
    const bool    sensitive = result.card.sensitive;
    auto finish = [self, key, target, file, mime, loop, sensitive](int w, int h, qint64 duration, const QString& posterFile) {
        if (!self || self->m_down)
            return;
        auto it = self->m_local.find(key);
        if (it == self->m_local.end() || it->state != linkstate::Local::Loading)
            return;
        // 2.5: a silent loop plays like a GIF (MediaLink::loop); a page marked for adults gets the spoiler cover.
        const MediaLink link = linkstate::webLink(target, mime, QFileInfo(file).size(), w, h, duration, loop, sensitive);
        const QString   mk   = self->m_core->adoptLocalFile(link, file, posterFile);
        if (mk.isEmpty()) {
            it->state = linkstate::Local::Failed;
            it->error = i18n::t("Couldn't show this file");
        } else {
            it->state    = linkstate::Local::Loaded;
            it->mediaKey = mk;
        }
        self->scheduleRefresh();
    };
    if (!result.thumbnailFile.isEmpty() && result.width > 0 && result.height > 0) {
        finish(result.width, result.height, result.durationMs, result.thumbnailFile);
        return;
    }
    // Joined in our destructor (m_pool.waitForDone), so it never outlives the plugin.
    m_pool.start([self, finish, file, poster] {
        const LocalMediaInfo info  = probeLocalMedia(file, true);
        QString              saved;
        if (!info.preview.isNull()) {
            const QByteArray jpeg = encodePreviewJpeg(info.preview);
            QFile            out(poster);
            if (!jpeg.isEmpty() && out.open(QIODevice::WriteOnly) && out.write(jpeg) == jpeg.size())
                saved = poster;
        }
        const int    w = info.width;
        const int    h = info.height;
        const qint64 d = info.durationMs;
        if (self)
            QMetaObject::invokeMethod(self.data(), [finish, w, h, d, saved] { finish(w, h, d, saved); }, Qt::QueuedConnection);
    });
}

void ChatLinkPreviews::onEntryChanged(const QString& key)
{
    if (m_down || !m_watched.contains(key))
        return;
    const MediaEntry* e     = m_core->entry(key);
    const int         state = e ? static_cast<int>(e->state) * 8 + static_cast<int>(e->previewState) : -1;
    // A state change of the file or its preview (downloaded, failed, gone from the cache) may change what a slot
    // shows (linkstate::serverFailed); anything else (progress, a thumbnail decoded) only its picture.
    if (m_lastState.value(key, -2) != state) {
        m_lastState.insert(key, state);
        scheduleRefresh();
    }
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && it->browser->isVisible())
            it->browser->viewport()->update();
    }
}

// ---- 2.4 auto-load ------------------------------------------------------------------------------------------

void ChatLinkPreviews::updateVisibleViewports()
{
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && it->browser->isVisible())
            it->browser->viewport()->update();
    }
}

ChatLinkPreviews::ChatContext ChatLinkPreviews::contextOf(QTextBrowser* browser) const
{
    using linkpreview::autoload::Chat;
    ChatContext ctx;
    if (!browser || !m_chat->m_textReactions)
        return ctx;
    const ChatTextReactions::ChatInfo info = m_chat->m_textReactions->chatInfo(browser);
    switch (info.kind) {
    case textreact::ChatKind::Server:
        ctx.chat = Chat::Server;
        break;
    case textreact::ChatKind::Channel:
        ctx.chat = Chat::Channel;
        break;
    case textreact::ChatKind::Private:
        ctx.chat = Chat::Private;
        break;
    case textreact::ChatKind::Unknown:
        break;
    }
    ctx.serverUid = info.serverUid;
    // The connection of a chat that is shown (the current tab's), and only if it is that chat's server.
    const uint64 sch = browser->isVisible() ? ts3::currentConnection() : 0;
    if (sch && ts3::isConnected(sch)) {
        const QString uid = ts3::serverUid(sch);
        if (info.serverUid.isEmpty() || info.serverUid == uid) {
            ctx.sch = sch;
            if (ctx.serverUid.isEmpty())
                ctx.serverUid = uid;
        }
    }
    return ctx;
}

bool ChatLinkPreviews::isOwn(const SlotRef& ref, const ChatContext& ctx) const
{
    return ctx.sch != 0 && !ref.senderUid.isEmpty() && ref.senderUid == ts3::ownUid(ctx.sch);
}

linkpreview::autoload::Presence ChatLinkPreviews::presentOf(quint64 sch, const QString& uid)
{
    // uidPresence walks every client of the server: once a second per sender at most. Both answers (connected, a
    // client in our channel) are always read, so a private chat's lookup is just as right for the channel chat.
    return m_present.get(sch, uid, wallMs(), [](quint64 server, const QString& who) {
        const ts3::UidPresence          found = ts3::uidPresence(server, who);
        linkpreview::autoload::Presence p;
        p.client       = found.client;
        p.inOurChannel = found.inOwnChannel;
        return p;
    });
}

linkpreview::autoload::Input ChatLinkPreviews::inputFor(const SlotRef& ref)
{
    using namespace linkpreview::autoload;
    const Settings&   s = Settings::instance();
    const ChatContext ctx = contextOf(ref.browser);
    Input             in;
    in.autoOn    = s.autoLoadLinkPreviews;
    in.makeOn    = s.showLinkPreviews && s.makeLinkPreviews;
    in.chat      = ctx.chat;
    in.timeSec   = ref.timeSec;
    in.nowSec    = wallSec();
    in.budgetSec = ref.budgetSec;
    in.own       = isOwn(ref, ctx);
    if (in.own) {
        QString hash;
        qint64  t = 0;
        if (m_pipeline && ref.timeSec >= 0 && textreact::parseLocalKey(ref.messageKey, &hash, &t)) {
            const linkpreview::SenderPipeline::OwnLink* rec = m_pipeline->ownLink(hash, t, ref.target.key);
            if (rec) {
                const bool pending = rec->state == linkpreview::SenderPipeline::OwnLink::State::Queued
                                     || rec->state == linkpreview::SenderPipeline::OwnLink::State::Fetching;
                in.ownRecord = pending ? OwnRecord::Pending : OwnRecord::Done;
            }
        }
        return in;
    }
    if (ctx.sch && !ref.senderUid.isEmpty() && (ctx.chat == Chat::Channel || ctx.chat == Chat::Private)) {
        in.senderHere = senderHere(ctx.chat, presentOf(ctx.sch, ref.senderUid));
        if (m_peers) {
            switch (m_peers->capabilityState(ctx.sch, ref.senderUid, capOf(ref.target))) {
            case peers::PeerDirectory::CapState::Yes:
                in.cap = SenderCap::Yes;
                break;
            case peers::PeerDirectory::CapState::No:
                in.cap = SenderCap::No;
                break;
            case peers::PeerDirectory::CapState::Checking:
                in.cap = SenderCap::Checking;
                break;
            case peers::PeerDirectory::CapState::Unknown:
                break;
            }
        }
    }
    in.skipped = m_hub && !ref.messageKey.isEmpty() && m_hub->isSkipped(ref.messageKey, ref.target.key);
    return in;
}

linkpreview::autoload::Wait ChatLinkPreviews::waitOf(const SlotRef& ref)
{
    if (!m_hub && !m_pipeline && !m_peers)
        return linkpreview::autoload::Wait::None; // no hub (tools): nothing to wait for
    return linkpreview::autoload::waitFor(inputFor(ref));
}

QString ChatLinkPreviews::senderKey(const ChatContext& ctx, const QString& senderUid)
{
    return ctx.serverUid + QLatin1Char('/') + (senderUid.isEmpty() ? QString::fromLatin1("?") : senderUid);
}

bool ChatLinkPreviews::autoEligible(const SlotRef& ref, const linkstate::LocalState& local, const linkstate::ServerState& server, bool ignoreBudget)
{
    const Settings& s = Settings::instance();
    if (m_down || m_stopped || !enabled() || !s.autoLoadLinkPreviews || m_hidden.contains(ref.slotId) || serverUsable(server))
        return false;
    if (local.state != linkstate::Local::Idle)
        return false; // loading, shown, failed (Retry is a click), too large or above the limit
    if (m_store && !ref.messageKey.isEmpty() && m_store->clearedEarlier(ref.messageKey, ref.target.key))
        return false; // its sender shared a preview (Clear cache took it): never from the website by itself
    const ChatContext ctx = contextOf(ref.browser);
    // 2.5: per link (a card's picture cap, the media cap otherwise; a web page needs "Show cards for other web pages").
    const qint64      cap = linkpreview::autoload::autoCap(ref.target, s, ctx.serverUid);
    if (cap <= 0)
        return false; // videos by default, the data saver, automatic picture downloads off: no request at all
    const auto hint = m_sizeHints.constFind(ref.target.key);
    if (hint != m_sizeHints.constEnd() && hint.value() > cap && ref.target.kind != linkpreview::Kind::Card)
        return false; // known to be larger (our pipeline found it so)
    if (contextFor(ref).senderNothing)
        return false; // 2.5: the sender's plugin found nothing there (or another website): never by itself
    if (gateOf(ref.target) != linkpreview::HostGate::Verdict::Go)
        return false; // 2.5: the site is cooling, walled or broken: no request (6.4)
    if (ignoreBudget)
        return true;
    // Its turn: a free slot, none of the sender's links loading, budget tokens for its requests.
    const QString sender = senderKey(ctx, ref.senderUid);
    return freeTurn(sender) && m_budget.has(sender, wallMs(), ref.target.expectedRequests);
}

bool ChatLinkPreviews::freeTurn(const QString& sender) const
{
    if (m_autoRuns.size() >= linkpreview::autoload::kMaxRunning)
        return false;
    int running = 0;
    for (const AutoRun& run : m_autoRuns)
        running += run.sender == sender ? 1 : 0;
    return running < linkpreview::autoload::kMaxPerSender;
}

qint64 ChatLinkPreviews::shownSize(const SlotRef& ref, const linkstate::LocalState& local) const
{
    if (local.state == linkstate::Local::OverAutoLimit)
        return local.size;
    // Our pipeline's "too large" (only when above the automatic limit: the card's tool tip says so).
    const qint64 hint = m_sizeHints.value(ref.target.key, -1);
    if (hint <= 0)
        return -1;
    const qint64 cap = linkpreview::autoload::autoCap(ref.target.kind, Settings::instance(), contextOf(ref.browser).serverUid);
    return hint > cap ? hint : -1;
}

bool ChatLinkPreviews::autoCandidate(const SlotRef& ref, const linkstate::Decision& d)
{
    if (d.media || d.fromServer)
        return false;
    if (d.wait == linkstate::Wait::Auto)
        return true;
    // 2.5: the look behind a quiet slot's Absent (the web class) counts.
    if (d.underlying != linkcard::Look::Click || d.wait != linkstate::Wait::None)
        return false;
    // A click card that may load by itself in its turn (held back, or no budget token yet).
    const linkstate::LocalState  l = slotLocal(ref, localOf(ref.target.key));
    const linkstate::ServerState s = serverOf(ref.messageKey, ref.target, false);
    return autoEligible(ref, l, s, true);
}

bool ChatLinkPreviews::askPartner(const SlotRef& ref, const ChatContext& ctx)
{
    using linkpreview::autoload::Chat;
    using Cap = peers::PeerDirectory::CapState;
    if (ctx.chat != Chat::Private || !ctx.sch || !m_peers || ref.senderUid.isEmpty())
        return false;
    const Cap cap = m_peers->capabilityState(ctx.sch, ref.senderUid, capOf(ref.target));
    if (cap != Cap::Unknown && cap != Cap::No)
        return false;
    // Only while their preview could still come for this message (an old one isn't worth a question). The wait it
    // would have if they made it: a link they don't make (cap No) had none in the queue model.
    if (!linkpreview::autoload::worthAsking(inputFor(ref), ref.target))
        return false;
    const linkpreview::autoload::Presence p = presentOf(ctx.sch, ref.senderUid);
    if (!p.client)
        return false;
    // Nobody asked them yet: ask (at most once a minute per person). They said they don't make previews: ask again
    // when that is older than a minute (they may have switched it on outside our channel). While asked, it waits.
    if (cap == Cap::Unknown)
        m_peers->checkPartner(ctx.sch, p.client);
    else
        m_peers->recheckPartner(ctx.sch, p.client);
    return m_peers->capabilityState(ctx.sch, ref.senderUid, capOf(ref.target)) == Cap::Checking;
}

void ChatLinkPreviews::runAutoLoads()
{
    const QVector<AutoWant> wanted = std::exchange(m_autoWanted, {});
    if (m_down || m_stopped || !enabled() || !Settings::instance().autoLoadLinkPreviews)
        return;
    if (m_autoHeld.size() > kMaxHeld)
        m_autoHeld.clear();
    bool changed = false;
    for (const AutoWant& w : wanted) {
        SlotRef ref;
        if (!w.browser || !w.browser->isVisible())
            continue;
        // 2.5: a quiet slot has no object: found again by its id.
        if (w.object.isEmpty() ? !refOfSlot(w.browser, w.slotId, &ref) : !refOf(w.browser, w.object, &ref))
            continue;
        if (!autoCandidate(ref, decisionFor(ref, false))) {
            changed = m_autoHeld.remove(ref.slotId) || changed; // changed meanwhile (an LP, hidden, the setting, a load)
            continue;
        }
        const ChatContext ctx = contextOf(ref.browser);
        if (askPartner(ref, ctx)) {
            changed = true; // it waits for their answer now
            continue;
        }
        // In its turn: one load per sender, kMaxRunning in all, the sender's budget. Otherwise it is held: a click card
        // (a click loads it at once) until a later pass starts it, newest first.
        const QString sender = senderKey(ctx, ref.senderUid);
        // 2.5: a load costs its link's requests (a card: the page and its picture).
        if (!freeTurn(sender) || !m_budget.take(sender, wallMs(), ref.target.expectedRequests)) {
            if (!m_autoHeld.contains(ref.slotId)) {
                m_autoHeld.insert(ref.slotId);
                changed = true;
            }
            continue;
        }
        m_autoHeld.remove(ref.slotId);
        startFetch(ref.target, Origin::Auto, ctx.serverUid, sender);
    }
    if (changed)
        updateVisibleViewports();
}

void ChatLinkPreviews::onOwnLinkChanged(const QString& urlKey)
{
    if (m_down || m_stopped || !enabled() || !m_pipeline)
        return;
    applyOwnRecord(urlKey);
    scheduleRefresh();
    updateVisibleViewports();
}

void ChatLinkPreviews::applyOwnRecord(const QString& urlKey)
{
    using State = linkpreview::SenderPipeline::OwnLink::State;
    if (m_down || m_stopped || !m_pipeline)
        return;
    const linkpreview::SenderPipeline::OwnLink* rec = m_pipeline->latestOwnLink(urlKey);
    // Applied once per (job, state): the pull from reconcile() can never loop, and a file a later Clear cache took
    // away is never shown again from an old record.
    if (!rec || (rec->jobId != 0 && m_ownApplied.value(rec->jobId, -1) == static_cast<int>(rec->state)))
        return;
    if (rec->jobId != 0) {
        m_ownApplied.insert(rec->jobId, static_cast<int>(rec->state));
        while (m_ownApplied.size() > kMaxOwnApplied) {
            const QList<int> ids = m_ownApplied.keys();
            m_ownApplied.remove(*std::min_element(ids.begin(), ids.end()));
        }
    }
    const linkstate::LocalState l              = m_local.value(urlKey);
    const bool                  waitsOnPipeline = m_pipelineWaits.contains(urlKey);
    switch (rec->state) {
    case State::Fetched: {
        if (l.state == linkstate::Local::Loaded)
            break; // shown already (a click or the cache)
        if (l.state == linkstate::Local::Loading && !waitsOnPipeline)
            cancelFetch(urlKey); // our own click or automatic load of it: the pipeline's download is here first
        m_pipelineWaits.remove(urlKey);
        m_targets.insert(urlKey, rec->target);
        linkstate::LocalState loading;
        loading.state = linkstate::Local::Loading;
        m_local.insert(urlKey, loading);
        ts3::log(LogLevel_INFO, 0, "Link preview: your %1 from %2 shown from your own download", {ts3::pub(kindText(rec->target.kind)), ts3::name(rec->target.host)});
        const linkpreview::Result result = rec->result; // the record may change while it is shown
        applyFetched(urlKey, result, false, true);       // the click path: a picture, a GIF, a video, the YouTube card
        break;
    }
    case State::Failed:
        if (rec->result.error == QLatin1String(linkpreview::errors::kTooLarge)) {
            if (rec->result.size > 0)
                m_sizeHints.insert(urlKey, rec->result.size); // never asked again just to learn the size
            if (waitsOnPipeline)
                cancelFetch(urlKey); // back to the normal rules
        } else if (l.state == linkstate::Local::Idle || waitsOnPipeline) {
            // The neutral error with Retry (no automatic second request; Retry is a click). 2.5: a page with nothing to
            // show is no slot at all.
            m_pipelineWaits.remove(urlKey);
            if (rec->result.error == QLatin1String(linkpreview::errors::kWebOff) && Settings::instance().showWebCards) {
                m_local.remove(urlKey); // review fix: cards for other web pages went on meanwhile: the normal rules
                break;
            }
            linkstate::LocalState failed;
            failed.state = linkstate::isNothingError(rec->result.error) ? linkstate::Local::Nothing : linkstate::Local::Failed;
            failed.error = linkstate::errorText(rec->result.error);
            m_targets.insert(urlKey, rec->target);
            m_local.insert(urlKey, failed);
            if (rec->result.error == QLatin1String(linkpreview::errors::kWebOff))
                m_webOff.insert(urlKey); // loaded again once cards for other web pages are on
        }
        break;
    case State::Skipped:
    case State::Reused:
        if (waitsOnPipeline)
            cancelFetch(urlKey); // the normal rules (Reused: the store has our LP)
        break;
    case State::Queued:
    case State::Fetching:
        break;
    }
}

void ChatLinkPreviews::onSkipped(const QString& urlKey)
{
    Q_UNUSED(urlKey);
    if (m_down || m_stopped)
        return;
    m_present.clear();
    updateVisibleViewports(); // the slot waits no more: it may load now
}

void ChatLinkPreviews::onPresenceChanged(quint64 sch)
{
    Q_UNUSED(sch);
    if (m_down || m_stopped)
        return;
    m_present.clear();
    if (m_waitTimer->isActive())
        updateVisibleViewports(); // a sender we wait for left, said BYE or changed what they make
}

void ChatLinkPreviews::showHeldCard(const linkpreview::Target& target)
{
    // Live test fix: our own message's card from another website (golang.org -> go.dev) that an automatic or cached
    // load held back, typically after a restart (our pipeline's record of it is gone): shown from the cache, as ours.
    // Read once: a record no longer in the cache leaves the slot as it was (no request, no loop).
    const auto it = m_local.find(target.key);
    if (m_down || m_stopped || it == m_local.end() || !linkstate::ownShowsHeldCard(*it, true))
        return;
    it->heldCrossSite = false;
    linkpreview::Result r;
    if (!linkpreview::Fetcher::cached(target, linksDir(), &r) || !r.ok || r.kind != linkpreview::Kind::Card)
        return;
    linkstate::LocalState loading;
    loading.state = linkstate::Local::Loading;
    m_local.insert(target.key, loading);
    m_targets.insert(target.key, target);
    ts3::log(LogLevel_INFO, 0, "Link preview: your card from %1 shown from the cache", {ts3::name(target.host)});
    applyFetched(target.key, r, true, true);
}

void ChatLinkPreviews::tryCache(const linkpreview::Target& target, bool own)
{
    if (m_down || m_stopped || !enabled() || target.key.isEmpty())
        return;
    linkpreview::Result r;
    if (!linkpreview::Fetcher::cached(target, linksDir(), &r)) {
        m_cacheMiss.insert(target.key);
        return;
    }
    if (!r.ok) {
        // 2.5: a fresh "nothing to show" record (a page without a card, a wall for 30 minutes): no slot, no request (a
        // click, Load link previews, still asks again).
        linkstate::LocalState nothing;
        nothing.state = linkstate::Local::Nothing;
        m_local.insert(target.key, nothing);
        m_targets.insert(target.key, target);
        return;
    }
    linkstate::LocalState loading;
    loading.state = linkstate::Local::Loading;
    m_local.insert(target.key, loading);
    m_targets.insert(target.key, target);
    ts3::log(LogLevel_INFO, 0, "Link preview: %1 from %2 shown from the cache", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    applyFetched(target.key, r, true, own); // no request, no budget, no limit: nothing is downloaded
}

void ChatLinkPreviews::onAnnounced(const linkpreview::Announcement& a)
{
    m_upgraded.insert(a.urlKey);
    scheduleRefresh(); // the card of that message's link becomes the sender's preview
}

void ChatLinkPreviews::stopUnneededFetches()
{
    // A sender's preview arrived while a click here was still loading that link from the website: once no slot
    // shows that load any more (each shows the sender's preview, or is hidden), it stops. Nothing more comes from
    // the website, and the slot never swaps from the sender's preview to this one when it would have finished.
    const QSet<QString> keys = m_upgraded;
    m_upgraded.clear();
    if (m_down)
        return;
    for (const QString& urlKey : keys) {
        if (m_local.value(urlKey).state != linkstate::Local::Loading)
            continue;
        bool wanted = false;
        for (auto it = m_views.constBegin(); it != m_views.constEnd() && !wanted; ++it) {
            for (const SlotRef& r : slotsOf(it.key())) {
                if (r.slot.urlKey != urlKey)
                    continue;
                const linkstate::Decision d = decisionFor(r, false);
                if (!d.media && d.look == linkcard::Look::Loading && d.wait == linkstate::Wait::None) { // a waiting strip doesn't show it
                    wanted = true;
                    break;
                }
            }
        }
        if (!wanted) {
            const linkpreview::Target target = m_targets.value(urlKey);
            ts3::log(LogLevel_INFO, 0, "Link preview: the sender's preview came first; loading %1 from %2 stopped",
                     {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
            cancelFetch(urlKey);
        }
    }
}

// ---- 2.5: Load link previews, the sender's card pictures, expired links, diagnostics -------------------------------

QAction* ChatLinkPreviews::menuAction(QMenu* menu, QTextBrowser* browser, int block)
{
    if (!menu || !browser || block < 0 || m_down || m_stopped || !enabled())
        return nullptr;
    // A link of that message that shows nothing (quiet, nothing found, the sender's "nothing", held) or an error.
    bool any = false;
    QVector<SlotRef> all = slotsOf(browser);
    if (const auto v = m_views.find(browser); v != m_views.end())
        all += v->quiet;
    for (const SlotRef& r : qAsConst(all)) {
        if (r.block != block || (r.target.cls == linkpreview::LinkClass::Web && !Settings::instance().showWebCards))
            continue;
        const linkstate::Decision d = decisionFor(r, false);
        any = any || (!d.media && (d.look == linkcard::Look::Absent || d.look == linkcard::Look::Error || d.look == linkcard::Look::Click));
    }
    if (!any)
        return nullptr;
    auto* action = new QAction(i18n::t("Load link &previews"), menu);
    const QString tip = i18n::t("Load the previews of this message's links. This connects your PC to their websites, which see your IP address.");
    action->setToolTip(tip);
    action->setStatusTip(tip);
    menu->setToolTipsVisible(true);
    const QPointer<ChatLinkPreviews> self(this);
    const QPointer<QTextBrowser>     guard(browser);
    connect(action, &QAction::triggered, this, [self, guard, block] {
        if (self && guard)
            self->loadMessageLinks(guard.data(), block);
    });
    return action;
}

void ChatLinkPreviews::loadMessageLinks(QTextBrowser* browser, int block)
{
    if (!browser || m_down || m_stopped || !enabled())
        return;
    QVector<SlotRef> all = slotsOf(browser);
    if (const auto v = m_views.find(browser); v != m_views.end())
        all += v->quiet;
    int started = 0;
    for (const SlotRef& r : qAsConst(all)) {
        if (r.block != block || started >= linkpreview::kMaxSlots)
            continue;
        if (r.target.cls == linkpreview::LinkClass::Web && !Settings::instance().showWebCards)
            continue; // never a class the user switched off
        const linkstate::Decision d = decisionFor(r, false);
        if (d.media || (d.look != linkcard::Look::Absent && d.look != linkcard::Look::Error && d.look != linkcard::Look::Click && d.look != linkcard::Look::Loading))
            continue;
        // A busy or walled site: the busy line at once, no request (a click never passes a cooldown or a wall).
        const linkpreview::HostGate::Verdict gate = gateOf(r.target);
        if (gate == linkpreview::HostGate::Verdict::Cooling || gate == linkpreview::HostGate::Verdict::Walled) {
            m_local.insert(r.target.key, linkstate::clickedUnderGate(gate)); // a wall: "No preview for this page"
            m_targets.insert(r.target.key, r.target);
            continue;
        }
        // A link loading by itself: kept, now the user's (R26); a cached "nothing" or an error: asked again.
        const linkstate::Local state = m_local.value(r.target.key).state;
        if (state == linkstate::Local::Nothing || state == linkstate::Local::Failed || state == linkstate::Local::Cooling)
            m_local.remove(r.target.key);
        m_cacheMiss.insert(r.target.key); // the click fetches (a cached "nothing" is passed by)
        startFetch(r.target, Origin::Click);
        ++started;
    }
    scheduleRefresh();
    updateVisibleViewports();
}

void ChatLinkPreviews::queueLpxPicture(const SlotRef& ref, const QString& mainKey)
{
    if (mainKey.isEmpty() || m_down || m_stopped)
        return;
    const MediaEntry* e = m_core->entry(mainKey);
    if (e && (e->state == MediaState::Ready || e->state == MediaState::Downloading || e->state == MediaState::Queued || e->state == MediaState::Failed))
        return; // Core has it, or is on it
    const ChatContext ctx = contextOf(ref.browser);
    if (!ctx.sch)
        return; // only for the chat that is shown, on its connection
    for (int i = 0; i < m_lpxPictures.size(); ++i) {
        if (m_lpxPictures.at(i).mainKey == mainKey) {
            m_lpxPictures.move(i, 0); // newest first
            return;
        }
    }
    m_lpxPictures.prepend({mainKey, ref.messageKey, ref.target.key, ctx.sch});
    while (m_lpxPictures.size() > kMaxLpxPictures)
        m_lpxPictures.removeLast();
    if (!m_waitTimer->isActive())
        m_waitTimer->start();
}

void ChatLinkPreviews::drainLpxPictures()
{
    // D37: at most one sender's card picture (or LPX media) per connection a tick, and only while the flood model keeps
    // the reserve that keeps the user's own chat posts free (uploads wait for it too).
    if (m_lpxPictures.isEmpty() || !m_store)
        return;
    QSet<quint64> served;
    for (int i = 0; i < m_lpxPictures.size();) {
        const LpxPicture p = m_lpxPictures.at(i);
        if (served.contains(p.sch)) {
            ++i;
            continue;
        }
        if (!ts3::isConnected(p.sch)) {
            m_lpxPictures.removeAt(i);
            continue;
        }
        if (!m_core->floodGovernor(p.sch).generalReady(FloodGovernor::Cost::Transfer, m_core->floodClockMs())) {
            served.insert(p.sch); // not now on this connection
            ++i;
            continue;
        }
        m_lpxPictures.removeAt(i);
        served.insert(p.sch);
        const linkpreview::Announcement* a = m_store->find(p.messageKey, p.urlKey);
        ServerFiles                      files;
        if (!a || !a->extended || !serverFilesOf(*a, &files) || files.main.key() != p.mainKey)
            continue;
        m_ensured.insert(p.mainKey);
        linkpreview::requestFiles(m_core, files.announcement); // the picture (or poster) at once, the media by the rules
    }
}

void ChatLinkPreviews::expiredCheck(const linkpreview::Target& target)
{
    // R22: a Discord attachment whose signed link is over shows nothing at once (no request, no wait).
    if (m_local.contains(target.key) || !linkpreview::net::expiredSignedLink(target.url, wallSec()))
        return;
    linkstate::LocalState nothing;
    nothing.state = linkstate::Local::Nothing;
    m_local.insert(target.key, nothing);
    m_targets.insert(target.key, target);
}

QStringList ChatLinkPreviews::diagnosticLines() const
{
    const Settings& s   = Settings::instance();
    QStringList     out;
    int             cap = linkpreview::kLpxShortCap;
    for (const uint64 sch : ts3::connections()) {
        if (ts3::isConnected(sch))
            cap = qMax(cap, linkpreview::lpxSendCap(m_core && m_core->serverIsTeamSpeakOwn(sch)));
    }
    out << i18n::t("Link previews: shown %1, web cards %2, loaded automatically %3, made %4; LPX cap %5")
               .arg(s.showLinkPreviews ? i18n::t("on") : i18n::t("off"))
               .arg(s.showWebCards ? i18n::t("on") : i18n::t("off"))
               .arg(s.autoLoadLinkPreviews ? i18n::t("on") : i18n::t("off"))
               .arg(s.makeLinkPreviews ? i18n::t("on") : i18n::t("off"))
               .arg(cap);
    if (m_hub) {
        const linkpreview::Hub::Counters c = m_hub->counters();
        out << i18n::t("Link previews received: LP %1 (accepted %2), LPNO %3, LPX %4 (accepted %5), LPXNO %6; rejected %7, rate-limited %8")
                   .arg(c.received)
                   .arg(c.accepted)
                   .arg(c.skipsReceived)
                   .arg(c.lpxReceived)
                   .arg(c.lpxAccepted)
                   .arg(c.lpxSkipsReceived)
                   .arg(c.rejected)
                   .arg(c.rateLimited);
    }
    if (m_pipeline) {
        const linkpreview::SenderPipeline::Counters c = m_pipeline->counters();
        out << i18n::t("Link previews sent: LP %1, LPX %2, LPNO %3, LPXNO %4 (nothing %5, other website %6), intake notices %7")
                   .arg(c.announced - c.lpxSent)
                   .arg(c.lpxSent)
                   .arg(c.skipNotices)
                   .arg(c.lpxSkips)
                   .arg(c.lpxSkipsNothing)
                   .arg(c.lpxSkipsCrossSite)
                   .arg(c.intakeNotices);
    }
    out << i18n::t("Link previews held: budget %1, gate %2, turn %3, cap %4, sender-nothing %5")
               .arg(m_holdCounts.value(QStringLiteral("budget")))
               .arg(m_holdCounts.value(QStringLiteral("gate")))
               .arg(m_holdCounts.value(QStringLiteral("turn")))
               .arg(m_holdCounts.value(QStringLiteral("cap")))
               .arg(m_holdCounts.value(QStringLiteral("sender-nothing")));
    if (m_yt)
        out << m_yt->diagnosticLines(); // 2.5 YouTube player
    return out;
}

#ifdef TSMEDIA_TESTHOOKS
QString ChatLinkPreviews::testCommand(const QString& cmd, const QJsonObject& c)
{
    // 2.5 YouTube player: yt.state, yt.stop, yt.play / yt.popup {"index": n} (the visible chat's YouTube cards, 0-based),
    // yt.spike (one player window with a page from memory: no YouTube, no network).
    if (cmd.startsWith(QLatin1String("yt."))) {
        if (!m_yt)
            return cmd + QString::fromLatin1(": no player");
        if (cmd == QLatin1String("yt.state"))
            return QString::fromLatin1("yt.state: ") + m_yt->describe();
        if (cmd == QLatin1String("yt.stop")) {
            m_yt->stop(m_yt->activeSlot());
            return QString::fromLatin1("yt.stop: ") + m_yt->describe();
        }
        QTextBrowser* chat = m_chat->visibleChatBrowser();
        if (cmd == QLatin1String("yt.spike")) {
            QWidget* top = chat ? chat->window() : nullptr;
            if (!top)
                return QString::fromLatin1("yt.spike: no visible chat");
            void* const hwnd = reinterpret_cast<void*>(top->winId());
            return QString::fromLatin1("yt.spike: ") + m_yt->spike(hwnd, ytbackend::clientRectOnScreen(hwnd));
        }
        if (cmd != QLatin1String("yt.play") && cmd != QLatin1String("yt.popup"))
            return QString::fromLatin1("unknown yt command ") + cmd;
        if (!chat)
            return cmd + QString::fromLatin1(": no visible chat");
        QVector<SlotRef> cards;
        for (const SlotRef& r : slotsOf(chat)) {
            if (r.slot.isCard() && r.target.kind == linkpreview::Kind::YouTube)
                cards << r;
        }
        const int index = c.value(QString::fromLatin1("index")).toInt(0);
        if (index < 0 || index >= cards.size())
            return cmd + QString::fromLatin1(": no YouTube card %1 (%2 in the visible chat)").arg(index).arg(cards.size());
        const SlotRef& ref = cards.at(index);
        m_yt->play(ref.slotId, ref.target.videoId, linkstate::webAddress(ref.slot.href, ref.target), cmd == QLatin1String("yt.popup"));
        return cmd + QString::fromLatin1(": ") + m_yt->describe();
    }
    // 2.5: the HostGate's state (groups and health keys: running, cooling hard / soft, broken, walled), no paths.
    if (cmd == QLatin1String("links.gate")) {
        const std::shared_ptr<linkpreview::HostGate> gate = m_fetcher ? m_fetcher->gate() : std::shared_ptr<linkpreview::HostGate>();
        if (!gate)
            return QString::fromLatin1("links.gate: no gate");
        return QString::fromLatin1("links.gate: running %1\n").arg(gate->runningCount()) + gate->describe().join(QLatin1Char('\n'));
    }
    // Every request, name lookup and refused connection of both Fetchers since the last links.net (then cleared).
    if (cmd == QLatin1String("links.net")) {
        const QStringList lines = linkpreview::nettrace::take();
        return QString::fromLatin1("links.net: %1 line(s)\n").arg(lines.size()) + lines.join(QLatin1Char('\n'));
    }
    QTextBrowser* browser = m_chat->visibleChatBrowser();
    if (!browser)
        return cmd + QString::fromLatin1(": no visible chat");
    if (cmd == QLatin1String("links.log"))
        return QString::fromLatin1("links.log: enabled %1, %2 slot(s)\n").arg(enabled() ? 1 : 0).arg(slotCount(browser))
               + describe(browser, c.value(QString::fromLatin1("find")).toString());
    if (cmd == QLatin1String("links.auto")) {
        QStringList running = m_autoRuns.keys();
        std::sort(running.begin(), running.end());
        return QString::fromLatin1("links.auto: auto %1, running %2 [%3], wanted %4, pipeline waits %5, cache misses %6, senders %7, size hints %8, held %9")
            .arg(Settings::instance().autoLoadLinkPreviews ? 1 : 0)
            .arg(m_autoRuns.size())
            .arg(running.join(QLatin1Char(',')))
            .arg(m_autoWanted.size())
            .arg(m_pipelineWaits.size())
            .arg(m_cacheMiss.size())
            .arg(m_budget.size())
            .arg(m_sizeHints.size())
            .arg(m_autoHeld.size());
    }
    if (cmd != QLatin1String("links.click"))
        return QString::fromLatin1("unknown links command ") + cmd;
    QVector<SlotRef> cards;
    const QString    find = c.value(QString::fromLatin1("find")).toString(); // live pass B: the cards of the messages with this text
    for (const SlotRef& r : slotsOf(browser)) {
        if (r.slot.isCard() && (find.isEmpty() || browser->document()->findBlockByNumber(r.block).text().contains(find)))
            cards << r;
    }
    // With find, index counts from the newest such card (0: the newest).
    const int index = find.isEmpty() ? c.value(QString::fromLatin1("index")).toInt(0) : cards.size() - 1 - c.value(QString::fromLatin1("index")).toInt(0);
    if (index < 0 || index >= cards.size())
        return cmd + QString::fromLatin1(": no card %1 (%2 in the visible chat)").arg(index).arg(cards.size());
    const SlotRef    ref   = cards.at(index);
    const QTextBlock block = browser->document()->findBlockByNumber(ref.block);
    const QRectF     br    = browser->document()->documentLayout()->blockBoundingRect(block);
    QScrollBar*      bar   = browser->verticalScrollBar();
    if (br.top() < bar->value() || br.bottom() > bar->value() + browser->viewport()->height())
        bar->setValue(qBound(bar->minimum(), qRound(br.top()) - 40, bar->maximum()));
    const QRectF              rect  = objectRect(browser, ref.slot.object, ref.slot.size);
    const PreviewStyle        style = styleOf(browser);
    const linkstate::Decision d     = decisionFor(ref, false);
    const linkcard::Card      card  = cardFor(ref, d, style);
    const linkcard::Layout    l     = linkcard::layout(card, style);
    const QString             zone  = c.value(QString::fromLatin1("zone")).toString(QString::fromLatin1("action"));
    QPointF                   local = l.action.isEmpty() ? QPointF(l.size.width() / 2.0, l.size.height() / 2.0) : l.action.center();
    if (zone == QLatin1String("hide"))
        local = l.hide.center();
    else if (zone == QLatin1String("body"))
        local = l.thumb.isEmpty() ? QPointF(l.text.left() + 4, l.size.height() / 2.0) : l.thumb.center();
    // 2.5 YouTube player live pass: the Consent state's parts and the card's other areas by name.
    const std::pair<const char*, QRectF> named[] = {{"consentPlay", l.consentPlay}, {"consentBrowser", l.consentBrowser},
                                                    {"terms", l.terms},             {"privacy", l.privacy},
                                                    {"question", l.question},       {"notice", l.notice},
                                                    {"title", l.title},             {"footer", l.footer},
                                                    {"play", l.play}};
    for (const auto& n : named) {
        if (zone != QLatin1String(n.first))
            continue;
        if (n.second.isEmpty())
            return QString::fromLatin1("%1: card %2 (%3) has no %4").arg(cmd).arg(index).arg(lookName(card.look), zone);
        local = n.second.center();
    }
    QWidget*     vp    = browser->viewport();
    const QPoint at    = (rect.topLeft() + local * m_chat->scaleOf(browser).s()).toPoint(); // 2.5 zoom: the layout is du
    const auto   mouse = [vp](QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(pos), QPointF(vp->mapTo(vp->window(), pos)), QPointF(vp->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(vp, &e);
    };
    mouse(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
    if (c.value(QString::fromLatin1("tip")).toBool()) { // live pass A: the card's tool tip there, no click
        QHelpEvent tip(QEvent::ToolTip, at, vp->mapToGlobal(at));
        QApplication::sendEvent(vp, &tip);
        return QString::fromLatin1("%1: card %2 (%3) at %4,%5: tool tip shown %6, rich %7: \"%8\"")
            .arg(cmd)
            .arg(index)
            .arg(lookName(card.look))
            .arg(at.x())
            .arg(at.y())
            .arg(QToolTip::isVisible() ? 1 : 0)
            .arg(Qt::mightBeRichText(QToolTip::text()) ? 1 : 0)
            .arg(QToolTip::text());
    }
    QString out = QString::fromLatin1("%1: card %2 (%3) at %4,%5: hover zone %6, cursor %7")
                      .arg(cmd)
                      .arg(index)
                      .arg(lookName(card.look))
                      .arg(at.x())
                      .arg(at.y())
                      .arg(static_cast<int>(m_hover.zone))
                      .arg(static_cast<int>(vp->cursor().shape()));
    if (c.value(QString::fromLatin1("button")).toString() == QLatin1String("right")) { // live pass B: the card's menu
        mouse(QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton);
        QCoreApplication::postEvent(vp, new QContextMenuEvent(QContextMenuEvent::Mouse, at, vp->mapToGlobal(at), Qt::NoModifier));
        return out + QString::fromLatin1("; right-click, the menu opens from the event loop");
    }
    mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    return out + QString::fromLatin1("; now: ") + lookName(decisionFor(ref, false).look);
}
#endif
