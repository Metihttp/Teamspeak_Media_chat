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
#include "chatreplies.h"
#include "chatscroll.h"
#include "chattextreactions.h"
#include "i18n.h"
#include "layoutformat.h"
#include "linkpreview/autoload.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/lphub.h" // usePluginLog, linksCacheDir, Hub
#include "linkpreview/previewstore.h"
#include "linkpreview/senderpipeline.h"
#include "mediaprobe.h"
#include "peers.h"
#include "settings.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "ts3api.h"

namespace {

constexpr int kGapTop     = 4; // transparent room above and below a card in its picture
constexpr int kGapBottom  = 4;
constexpr int kMaxThumbs  = 48;
constexpr int kSpinMs     = 50;
constexpr int kAutoDelayMs   = 100;  // 2.4 auto-load: the queue runs this long after a paint saw candidates
constexpr int kWaitTickMs    = 1000; // ... and the waits are looked at again this often while one is on screen
constexpr int kMaxHeld       = 1024; // ... slots held back at most (more: all forgotten, looked at again)
constexpr int kMaxOwnApplied = 64;

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

// The sender's preview is usable (linkstate::decide's rule 3).
bool serverUsable(const linkstate::ServerState& server)
{
    return server.announced && !linkstate::serverFailed(server) && !server.mainKey.isEmpty();
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
    }
    return {};
}

QString kindText(linkpreview::Kind kind)
{
    return QString::fromLatin1(linkpreview::kindName(kind)); // net-core's names ("youtube", "image", ...)
}

bool spinning(const linkcard::Card& card)
{
    return (card.look == linkcard::Look::Loading && card.progress < 0) || (card.thumbnail.isNull() && card.thumbnailLoading);
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
    // "chat"; which of the two a load was is logged when it starts ("(clicked)" / "(automatic)").
    m_fetcher = new linkpreview::Fetcher(this);
    linkpreview::usePluginLog(m_fetcher, "chat");
    m_loader  = [this](const linkpreview::Target& target, qint64 maxBytes, const QString& cacheDir) {
        if (m_fetcher)
            m_fetcher->load(target, maxBytes, cacheDir);
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
        // A visible slot waits (for us, a sender, a budget token): its time or the sender may have changed.
        if (!m_waitSeen || m_down || m_stopped) {
            m_waitTimer->stop();
            return;
        }
        m_waitSeen = false;
        m_present.clear();
        updateVisibleViewports();
    });
    pruneLinkCache(); // what earlier sessions left over the cap
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
        if (it->state == linkstate::Local::Loading)
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
            } else if (!it->thumbnailFile.isEmpty()) {
                gone = !QFileInfo::exists(it->thumbnailFile);
            }
        }
        if (gone)
            it = m_local.erase(it);
        else
            ++it;
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

void ChatLinkPreviews::setLoader(std::function<void(const linkpreview::Target&, qint64, const QString&)> loader)
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

void ChatLinkPreviews::fillMessage(SlotRef* ref, linkpreview::autoload::SenderQueue* queue)
{
    QString hash;
    qint64  t = 0;
    ref->timeSec   = !ref->messageKey.isEmpty() && textreact::parseLocalKey(ref->messageKey, &hash, &t) ? t : -1;
    // The sender makes one preview at a time across their messages: this link waits for theirs before it too.
    ref->budgetSec = queue ? queue->add(ref->senderUid, ref->timeSec, ref->target.kind) : 0;
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
    if (!a || !linkstate::kindFits(a->kind, target.kind))
        return s;
    ServerFiles files;
    if (!serverFilesOf(*a, &files))
        return s;
    s.announced = true;
    s.hasMedia  = files.hasMedia;
    s.mainKey   = files.main.key();
    m_serverKeys.insert(s.mainKey);
    if (ensure && !m_ensured.contains(s.mainKey)) {
        m_ensured.insert(s.mainKey);
        // From the TeamSpeak server, checked against its SHA-256: the preview always, the full file by the user's
        // automatic download rules (the hub asked already when the announcement came; Core knows it then).
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
        if (known > maxBytesFor(ref.target.kind)) {
            local.state = Local::TooLarge;
            local.size  = known;
        }
    }
    return local;
}

linkstate::Decision ChatLinkPreviews::decisionFor(const SlotRef& ref, bool ensure)
{
    const linkstate::LocalState  local  = slotLocal(ref, localOf(ref.target.key));
    const linkstate::ServerState server = serverOf(ref.messageKey, ref.target, ensure);
    const bool                   hidden = m_hidden.contains(ref.slotId);
    // 2.4 auto-load: what it waits for matters only when nothing else is shown (decide's rule 5).
    linkstate::Wait wait = linkstate::Wait::None;
    const bool      bare = local.state == linkstate::Local::Idle || local.state == linkstate::Local::OverAutoLimit
                      || (local.state == linkstate::Local::Loaded && local.mediaKey.isEmpty() && ref.target.kind != linkpreview::Kind::YouTube);
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
    return linkstate::decide(ref.target.kind, hidden, local, server, wait);
}

linkcard::Card ChatLinkPreviews::cardFor(const SlotRef& ref, const linkstate::Decision& d, const PreviewStyle& style)
{
    Q_UNUSED(style);
    linkcard::Card c;
    c.look = d.look;
    c.kind = ref.target.kind;
    c.host = linkcard::cleanText(ref.target.host, 120);
    const linkstate::LocalState l = slotLocal(ref, m_local.value(ref.target.key));
    switch (d.look) {
    case linkcard::Look::Loading:
        c.progress = d.wait == linkstate::Wait::None ? l.progress : -1.0;
        switch (d.wait) {
        case linkstate::Wait::Own:
            c.waiting = linkcard::Waiting::Own;
            break;
        case linkstate::Wait::Sender:
            c.waiting = linkcard::Waiting::Sender;
            break;
        case linkstate::Wait::Checking:
            c.waiting = linkcard::Waiting::Checking;
            break;
        case linkstate::Wait::Auto:
        case linkstate::Wait::None:
            c.waiting = linkcard::Waiting::None;
            break;
        }
        break;
    case linkcard::Look::Click:
        c.size          = shownSize(ref, l); // above the automatic limit: "Video · 38 MB"
        c.overAutoLimit = l.state == linkstate::Local::OverAutoLimit || c.size > 0;
        if (c.overAutoLimit) // the site didn't say its size: "Video · over 15 MB"
            c.autoLimit = linkpreview::autoload::autoCap(ref.target.kind, Settings::instance(), contextOf(ref.browser).serverUid);
        break;
    case linkcard::Look::Error:
        c.error = linkcard::cleanText(l.error, 160);
        break;
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
    const linkstate::LocalState l = m_local.value(ref.target.key);
    if (l.thumbnailFile.isEmpty())
        return {};
    const QString cacheKey = l.thumbnailFile + QLatin1Char('@') + QString::number(pixels.width()) + QLatin1Char('x') + QString::number(pixels.height());
    if (m_thumbs.contains(cacheKey))
        return m_thumbs.value(cacheKey);
    QImage       image;
    QImageReader reader(l.thumbnailFile);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
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
        ref.messageKey = messageKeyOf(browser, link.block, &days, &ref.senderUid);
        ref.slotId     = ref.messageKey.isEmpty() ? slotIdOf(browser, link.block, link.target.key, &days, &ref.senderUid)
                                                  : ref.messageKey + QLatin1Char('|') + link.target.key;
        fillMessage(&ref, &queue);
        // 2.4 auto-load, before the decision: our own pipeline's download (the signal may have come before the slot
        // did), then the links folder (no request; every link of the chat, so a slot has its final size at once).
        if (isOwn(ref, ctx))
            applyOwnRecord(link.target.key);
        if (autoOn && !m_local.contains(link.target.key) && !m_cacheMiss.contains(link.target.key) && !serverOf(ref.messageKey, link.target, false).announced
            && !(m_pipeline && m_pipeline->fetching(link.target.key)))
            tryCache(link.target);
        const linkstate::Decision d     = decisionFor(ref, true);
        const linkdoc::Slot       slot  = linkdoc::slotAfter(doc->findBlockByNumber(link.block), link);
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
                edited = linkdoc::insertSlot(doc, links.at(e.link), e.name, e.size) || edited;
                break;
            case 1: {
                QTextCursor c(doc);
                c.setPosition(e.position);
                c.setPosition(e.position + 1, QTextCursor::KeepAnchor);
                const QString was = c.charFormat().toImageFormat().name();
                if (linkdoc::replaceObject(doc, e.position, e.name, e.size)) {
                    edited = true;
                    if (was.startsWith(linkdoc::cardPrefix()))
                        gone << was;
                }
                break;
            }
            case 2:
                edited = linkdoc::resizeObject(doc, e.position, e.size) || edited;
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
        view->rendered.remove(name);
    }
    view->refsValid = false;
    if (!edited)
        return;
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
        for (const QString& key : automatic)
            cancelFetch(key);
        m_autoWanted.clear();
        m_autoHeld.clear();
        m_autoTimer->stop();
    } else {
        limitsChanged();
    }
    m_present.clear();
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
        const qint64  cap = linkpreview::autoload::autoCap(m_targets.value(key).kind, s, run.serverUid);
        if (cap >= run.maxBytes || (cap > 0 && run.total >= 0 && run.total <= cap))
            continue; // the same or a higher limit, or the file fits the lower one: it goes on
        const bool   over = cap > 0 && (run.total > cap || run.received > cap);
        const qint64 size = run.total;
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 stopped (a lower automatic limit or the data saver)",
                 {ts3::pub(kindText(m_targets.value(key).kind)), ts3::name(m_targets.value(key).host)});
        cancelFetch(key);
        stopped = true;
        if (over) {
            // Larger than the new limit: a click card with its size (no second request to learn it).
            linkstate::LocalState l;
            l.state = linkstate::Local::OverAutoLimit;
            l.size  = size;
            m_local.insert(key, l);
        }
        // Otherwise (data saver, or a size still unknown) it starts over by the new rules, if at all.
    }
    if (stopped)
        scheduleRefresh(); // its card's size may change (a click card with a size)
    updateVisibleViewports();
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
    ChatTextReactions::KeyDays         days;  // one reading of the chat's days for every slot
    linkpreview::autoload::SenderQueue queue; // 2.4 auto-load: the senders' links so far, in the chat's order
    for (QTextBlock b = view.document->begin(); b.isValid(); b = b.next()) {
        for (const linkdoc::Slot& s : linkdoc::slotsIn(b)) {
            SlotRef ref;
            ref.browser = browser;
            ref.slot    = s;
            ref.block   = b.blockNumber();
            // The slot's own link: links of one video share a key (youtu.be/x, www.youtube.com/watch?v=x), so the
            // one remembered for the key may have another host (live test: a www.youtube.com card said youtu.be
            // after a youtu.be link of that video in another chat). The remembered one only if this can't be read.
            ref.target = linkpreview::classify(s.href);
            if (ref.target.key != s.urlKey) {
                const linkpreview::Target known = m_targets.value(s.urlKey);
                if (!known.key.isEmpty())
                    ref.target = known;
            }
            ref.messageKey = messageKeyOf(browser, ref.block, &days, &ref.senderUid);
            ref.slotId     = ref.messageKey.isEmpty() ? slotIdOf(browser, ref.block, s.urlKey, &days, &ref.senderUid) : ref.messageKey + QLatin1Char('|') + s.urlKey;
            fillMessage(&ref, &queue);
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

int ChatLinkPreviews::slotCount(QTextBrowser* browser) const
{
    return slotsOf(browser).size();
}

QString ChatLinkPreviews::describe(QTextBrowser* browser, const QString& find) const
{
    QStringList lines;
    auto*       self = const_cast<ChatLinkPreviews*>(this);
    for (const SlotRef& r : slotsOf(browser)) {
        if (!find.isEmpty() && !browser->document()->findBlockByNumber(r.block).text().contains(find))
            continue;
        const linkstate::Decision   d = self->decisionFor(r, false);
        const linkstate::LocalState l = self->slotLocal(r, m_local.value(r.target.key));
        const qint64                size = d.look == linkcard::Look::Click ? shownSize(r, l) : d.look == linkcard::Look::TooLarge ? l.size : -1;
        lines << QString::fromLatin1("block %1 %2 %3 %4 server=%5 message=%6 slot=%7x%8")
                         .arg(r.block)
                         .arg(kindText(r.target.kind), r.target.host, d.media ? QString::fromLatin1("media") : lookName(d.look))
                         .arg(d.fromServer ? 1 : 0)
                         .arg(r.messageKey.isEmpty() ? QString::fromLatin1("-") : r.messageKey)
                         .arg(qRound(r.slot.size.width()))
                         .arg(qRound(r.slot.size.height()))
                     + QString::fromLatin1(" wait=%1 size=%2").arg(waitName(d.wait), size > 0 ? QString::number(size) : QString::fromLatin1("-"))
                     + (d.media ? QString::fromLatin1(" key=") + d.mediaKey.left(16) : QString());
    }
    return lines.join(QLatin1Char('\n'));
}

bool ChatLinkPreviews::ownsKey(const QString& key) const
{
    // Files from websites, and the files of senders' announcements (never ordinary uploads).
    return !key.isEmpty() && (m_core->isLocalFile(key) || m_serverKeys.contains(key));
}

// ---- drawing ------------------------------------------------------------------------------------------------

PreviewStyle ChatLinkPreviews::styleOf(QTextBrowser* browser) const
{
    PreviewStyle style = m_chat->styleFor(browser);
    // The layout's content column (Cozy / Compact indent) within the chat's margins; in a bubble its right
    // margin too, like the poll cards.
    const int indent = m_chat->m_layout ? m_chat->m_layout->indentFor(browser) : 0;
    const int margin = static_cast<int>(2 * browser->document()->documentMargin());
    const int right  = qMax(12, m_chat->m_layout ? m_chat->m_layout->bubbleRightFor(browser) + 2 : 0);
    style.maxWidth   = qMax(160, qMin(style.maxWidth, m_chat->previewAreaWidth(browser) - indent - margin - right));
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
    QStringList parts;
    parts << QString::number(style.dark) << style.font.toString() << QString::number(style.dpr) << QString::number(style.maxWidth)
          << (style.bubbles.isEmpty() ? QString() : style.bubbles.first().name()) << QString::number(static_cast<int>(card.look))
          << QString::number(static_cast<int>(card.kind)) << card.host << card.title << card.author << QString::number(card.progress, 'f', 2)
          << QString::number(static_cast<int>(card.waiting)) << card.error
          << QString::number(card.size) << QString::number(card.overAutoLimit) << QString::number(card.autoLimit) << QString::number(card.durationMs)
          << QString::number(card.thumbnail.isNull() ? 0 : card.thumbnail.cacheKey())
          << QString::number(card.thumbnailLoading) << QString::number(pointer.over) << QString::number(static_cast<int>(pointer.hover))
          << QString::number(static_cast<int>(pointer.pressed));
    if (spinning(card) && style.animate)
        parts << QString::number(QDateTime::currentMSecsSinceEpoch() / kSpinMs); // the ring turns
    return parts.join(QLatin1Char('|'));
}

QRectF ChatLinkPreviews::objectRect(QTextBrowser* browser, int position, const QSizeF& size) const
{
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
    const qreal  top       = qMax(line.y(), line.y() + line.ascent() - size.height()) + kGapTop;
    return QRectF(blockRect.left() + x - browser->horizontalScrollBar()->value(), blockRect.top() + top - browser->verticalScrollBar()->value(), size.width(),
                  size.height() - kGapTop - kGapBottom);
}

void ChatLinkPreviews::renderCard(QTextBrowser* browser, View& view, const SlotRef& ref)
{
    const PreviewStyle        style = styleOf(browser);
    const linkstate::Decision d     = decisionFor(ref, false);
    if (d.media)
        return; // the document will get the media (a reconcile is due)
    linkcard::Card card = cardFor(ref, d, style);
    if (card.look == linkcard::Look::YouTube || card.look == linkcard::Look::Poster) {
        const linkcard::Layout l = linkcard::layout(card, style);
        card.thumbnail           = thumbnailFor(ref, d, (l.thumb.size() * style.dpr).toSize());
        if (card.thumbnail.isNull() && d.fromServer) {
            const linkstate::ServerState s = serverOf(ref.messageKey, ref.target, false);
            const MediaEntry*            e = m_core->entry(s.mainKey);
            card.thumbnailLoading = e && (e->state == MediaState::Queued || e->state == MediaState::Downloading || (e->state == MediaState::Ready));
        }
    }
    const linkcard::Pointer pointer   = pointerFor(browser, ref.slot.name);
    const QString           signature = signatureOf(card, style, pointer);
    if (view.rendered.value(ref.slot.name) == signature)
        return;
    const QImage picture = linkcard::render(card, style, pointer);
    const qreal  dpr     = qMax<qreal>(1.0, style.dpr);
    QImage       padded(QSize(picture.width(), picture.height() + qRound((kGapTop + kGapBottom) * dpr)), QImage::Format_ARGB32_Premultiplied);
    padded.setDevicePixelRatio(dpr);
    padded.fill(Qt::transparent);
    {
        QPainter p(&padded);
        p.drawImage(QPointF(0, kGapTop), picture);
    }
    view.document->addResource(QTextDocument::ImageResource, QUrl(ref.slot.name), QPixmap::fromImage(padded));
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
    if (refs.isEmpty())
        return;
    const QRectF shown = QRectF(browser->viewport()->rect()).adjusted(0, -400, 0, 400);
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
    for (const SlotRef& ref : refs) {
        if (!ref.slot.isCard())
            continue;
        const QRectF rect = objectRect(browser, ref.slot.object, ref.slot.size);
        if (rect.intersects(shown)) {
            const linkstate::Decision d = decisionFor(ref, false);
            if (d.media || cardSize(cardFor(ref, d, style), style) != ref.slot.size.toSize()) {
                resize = true; // laid out again first (not while the chat paints)
                continue;
            }
            if (d.wait == linkstate::Wait::Auto && browser->isVisible()) {
                wanted.append({QPointer<QTextBrowser>(browser), ref.slot.name});
            } else if (d.wait == linkstate::Wait::Own || d.wait == linkstate::Wait::Sender || d.wait == linkstate::Wait::Checking) {
                m_waitSeen = true; // looked at again in a second (its time, the sender)
            } else if (autoOn && browser->isVisible() && autoCandidate(ref, d)) {
                // Held back, or no budget token yet: runAutoLoads starts it in its turn; looked at again in a second.
                wanted.append({QPointer<QTextBrowser>(browser), ref.slot.name});
                m_waitSeen = true;
            }
            renderCard(browser, *view, ref);
        } else if (view->rendered.contains(ref.slot.name) && !rect.intersects(far)) {
            // Far off screen: its memory goes (drawn again when it comes back).
            view->document->addResource(QTextDocument::ImageResource, QUrl(ref.slot.name), blankPicture());
            view->rendered.remove(ref.slot.name);
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
        spot.zone                       = linkcard::zoneAt(card, l, QPointF(pos) - rect.topLeft(), true);
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
        openInBrowser(linkstate::webAddress(ref.slot.href, ref.target)); // the link as written (a start time stays)
        break;
    case linkcard::Zone::Hide:
        setHidden(ref.slotId, ref.target.key, true);
        break;
    case linkcard::Zone::Show:
        setHidden(ref.slotId, ref.target.key, false);
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
    switch (card.look) {
    case linkcard::Look::Click:
        primary = menu->addAction(i18n::t("&Load preview"), this, [self, target] {
            if (self)
                self->startFetch(target);
        });
        break;
    case linkcard::Look::Error:
        primary = menu->addAction(i18n::t("Re&try"), this, [self, target] { // R is Reply's (below)
            if (self)
                self->startFetch(target);
        });
        break;
    case linkcard::Look::Hidden:
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
    // The message's own items: Add reaction (text reactions), Reply.
    if (m_chat->m_textReactions) {
        if (QAction* react = m_chat->m_textReactions->menuAction(menu, browser, ref.block)) {
            menu->addSeparator();
            menu->addAction(react);
        }
    }
    if (m_chat->m_replies)
        m_chat->m_replies->addPreviewMenu(menu, browser);
    menu->setDefaultAction(primary);
    menu->popup(globalPos);
}

bool ChatLinkPreviews::addMediaMenu(QMenu* menu, QTextBrowser* browser, const QString& key, const QPoint& viewportPos, QUrl* webUrl)
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
    if (!enabled() || m_down || m_stopped || target.key.isEmpty() || target.kind == linkpreview::Kind::None)
        return;
    if (m_local.value(target.key).state == linkstate::Local::Loading)
        return; // one fetch per link
    linkstate::LocalState l;
    l.state = linkstate::Local::Loading;
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
    const qint64 maxBytes = origin == Origin::Auto ? linkpreview::autoload::autoCap(target.kind, Settings::instance(), serverUid) : maxBytesFor(target.kind);
    if (origin == Origin::Auto) {
        AutoRun run;
        run.sender    = sender;
        run.serverUid = serverUid;
        run.maxBytes  = maxBytes;
        m_autoRuns.insert(target.key, run);
    }
    if (origin == Origin::Auto)
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 (automatic)", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    else
        ts3::log(LogLevel_INFO, 0, "Link preview: loading a %1 from %2 (clicked)", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    if (m_loader)
        m_loader(target, maxBytes, dir);
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
}

void ChatLinkPreviews::applyFetched(const QString& key, const linkpreview::Result& result, bool fromCache)
{
    auto it = m_local.find(key);
    if (m_down || it == m_local.end() || it->state != linkstate::Local::Loading)
        return; // canceled meanwhile
    if (!result.ok && result.error == QLatin1String(linkpreview::errors::kCanceled))
        return; // the answer to a cancel of ours (a load of the link started again since): its own answer follows
    const bool wasAuto = m_autoRuns.remove(key) > 0;
    m_pipelineWaits.remove(key);
    m_cacheMiss.remove(key);
    const linkpreview::Target target = m_targets.value(key);
    if (wasAuto)
        updateVisibleViewports(); // a running slot is free: the next on-screen pass may start one
    if (!result.ok) {
        if (result.error == QLatin1String(linkpreview::errors::kTooLarge) && wasAuto && !(result.size > maxBytesFor(target.kind))) {
            // 2.4 auto-load: above the automatic limit, nothing more of it is downloaded: a click card with its size
            // (or the limit, when the site doesn't say). Larger than a click may load: "Too large" below (no Load
            // preview that could only fail and ask the site once more).
            it->state = linkstate::Local::OverAutoLimit;
            it->size  = result.size;
        } else if (result.error == QLatin1String(linkpreview::errors::kTooLarge)) {
            it->state = linkstate::Local::TooLarge;
            it->size  = result.size; // what the website announced (-1: it didn't say)
        } else {
            it->state = linkstate::Local::Failed;
            it->error = linkstate::errorText(result.error); // neutral: the reason is in the log only
        }
        ts3::log(LogLevel_INFO, 0, "Link preview: %1 from %2 not shown (%3)", {ts3::pub(kindText(target.kind)), ts3::name(target.host), ts3::pub(result.error)});
        scheduleRefresh();
        return;
    }
    if (!fromCache)
        pruneLinkCache(); // the cache of website files stays within its cap (the oldest go first, never this one)
    it = m_local.find(key);
    if (it == m_local.end())
        return;
    linkstate::LocalState& l = it.value();
    switch (target.kind) {
    case linkpreview::Kind::YouTube:
        l.state         = linkstate::Local::Loaded;
        l.title         = linkcard::cleanText(result.title, 200);
        l.author        = linkcard::cleanText(result.author, 100);
        l.thumbnailFile = result.thumbnailFile;
        break;
    case linkpreview::Kind::Image:
    case linkpreview::Kind::Gif: {
        int w = result.width;
        int h = result.height;
        if (w <= 0 || h <= 0) {
            QImageReader reader(result.localFile);
            reader.setAutoTransform(true);
            reader.setDecideFormatFromContent(true);
            const QSize s = reader.size(); // the header only
            w             = s.width();
            h             = s.height();
        }
        const MediaLink link = linkstate::webLink(target, result.mime, QFileInfo(result.localFile).size(), w, h, 0);
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
        adoptVideo(target, result);
        return; // its poster first (a worker of ours), then the slot
    case linkpreview::Kind::None:
        l.state = linkstate::Local::Failed;
        break;
    }
    scheduleRefresh();
}

// A video loaded from a website: its poster, size and length from mediaprobe (Media Foundation) on our
// worker, unless the fetcher made a poster; then it becomes a Core entry and plays like an uploaded one.
void ChatLinkPreviews::adoptVideo(const linkpreview::Target& target, const linkpreview::Result& result)
{
    const QString file   = result.localFile;
    const QString mime   = result.mime;
    const QString poster = file + QString::fromLatin1(".poster.jpg");
    const QPointer<ChatLinkPreviews> self(this);
    const QString key = target.key;
    auto finish = [self, key, target, file, mime](int w, int h, qint64 duration, const QString& posterFile) {
        if (!self || self->m_down)
            return;
        auto it = self->m_local.find(key);
        if (it == self->m_local.end() || it->state != linkstate::Local::Loading)
            return;
        const MediaLink link = linkstate::webLink(target, mime, QFileInfo(file).size(), w, h, duration);
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
            switch (m_peers->capabilityState(ctx.sch, ref.senderUid, "l")) {
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
    const qint64      cap = linkpreview::autoload::autoCap(ref.target.kind, s, ctx.serverUid);
    if (cap <= 0)
        return false; // videos by default, the data saver, automatic picture downloads off: no request at all
    const auto hint = m_sizeHints.constFind(ref.target.key);
    if (hint != m_sizeHints.constEnd() && hint.value() > cap)
        return false; // known to be larger (our pipeline found it so)
    if (ignoreBudget)
        return true;
    // Its turn: a free slot, none of the sender's links loading, a budget token.
    const QString sender = senderKey(ctx, ref.senderUid);
    return freeTurn(sender) && m_budget.has(sender, wallMs());
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
    if (d.look != linkcard::Look::Click || d.wait != linkstate::Wait::None)
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
    const Cap cap = m_peers->capabilityState(ctx.sch, ref.senderUid, "l");
    if (cap != Cap::Unknown && cap != Cap::No)
        return false;
    // Only while their preview could still come for this message (an old one isn't worth a question).
    linkpreview::autoload::Input in = inputFor(ref);
    in.cap                          = linkpreview::autoload::SenderCap::Yes;
    if (linkpreview::autoload::waitFor(in) != linkpreview::autoload::Wait::Sender)
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
    return m_peers->capabilityState(ctx.sch, ref.senderUid, "l") == Cap::Checking;
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
        if (!w.browser || !w.browser->isVisible() || !refOf(w.browser, w.object, &ref))
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
        if (!freeTurn(sender) || !m_budget.take(sender, wallMs())) {
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
        applyFetched(urlKey, result, false);             // the click path: a picture, a GIF, a video, the YouTube card
        break;
    }
    case State::Failed:
        if (rec->result.error == QLatin1String(linkpreview::errors::kTooLarge)) {
            if (rec->result.size > 0)
                m_sizeHints.insert(urlKey, rec->result.size); // never asked again just to learn the size
            if (waitsOnPipeline)
                cancelFetch(urlKey); // back to the normal rules
        } else if (l.state == linkstate::Local::Idle || waitsOnPipeline) {
            // The neutral error with Retry (no automatic second request; Retry is a click).
            m_pipelineWaits.remove(urlKey);
            linkstate::LocalState failed;
            failed.state = linkstate::Local::Failed;
            failed.error = linkstate::errorText(rec->result.error);
            m_targets.insert(urlKey, rec->target);
            m_local.insert(urlKey, failed);
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

void ChatLinkPreviews::tryCache(const linkpreview::Target& target)
{
    if (m_down || m_stopped || !enabled() || target.key.isEmpty())
        return;
    linkpreview::Result r;
    if (!linkpreview::Fetcher::cached(target, linksDir(), &r) || !r.ok) {
        m_cacheMiss.insert(target.key);
        return;
    }
    linkstate::LocalState loading;
    loading.state = linkstate::Local::Loading;
    m_local.insert(target.key, loading);
    m_targets.insert(target.key, target);
    ts3::log(LogLevel_INFO, 0, "Link preview: %1 from %2 shown from the cache", {ts3::pub(kindText(target.kind)), ts3::name(target.host)});
    applyFetched(target.key, r, true); // no request, no budget, no limit: nothing is downloaded
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

#ifdef TSMEDIA_TESTHOOKS
QString ChatLinkPreviews::testCommand(const QString& cmd, const QJsonObject& c)
{
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
    QWidget*     vp    = browser->viewport();
    const QPoint at    = (rect.topLeft() + local).toPoint();
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
