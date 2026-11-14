#include "linkpreview/linkstate.h"

#include <QFileInfo>

#include "i18n.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/providers.h"

namespace linkstate {

namespace {

constexpr int kMaxSide = 16384;

int boundedSide(int v)
{
    return v > 0 && v <= kMaxSide ? v : 0;
}

QString extensionFor(const QString& mime, const linkpreview::Target& target)
{
    const QString m = mime.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    static const struct {
        const char* mime;
        const char* ext;
    } table[] = {{"image/gif", "gif"},   {"image/png", "png"},   {"image/jpeg", "jpg"},       {"image/jpg", "jpg"},
                 {"image/webp", "webp"}, {"video/mp4", "mp4"},   {"video/webm", "webm"},      {"video/quicktime", "mov"},
                 {"video/x-m4v", "m4v"}};
    for (const auto& row : table) {
        if (m == QLatin1String(row.mime))
            return QString::fromLatin1(row.ext);
    }
    const QString fromUrl = QFileInfo(target.url.path()).suffix().toLower();
    for (const char* ext : {"png", "jpg", "jpeg", "gif", "webp", "mp4", "webm", "mov", "m4v"}) {
        if (fromUrl == QLatin1String(ext))
            return fromUrl == QLatin1String("jpeg") ? QString::fromLatin1("jpg") : fromUrl;
    }
    switch (target.kind) {
    case linkpreview::Kind::Gif:
        return QString::fromLatin1("gif");
    case linkpreview::Kind::Video:
    case linkpreview::Kind::Loop:
        return QString::fromLatin1("mp4");
    default:
        return QString::fromLatin1("jpg");
    }
}

} // namespace

void readEntry(ServerState* server, const MediaEntry* entry)
{
    if (!server || !entry)
        return;
    server->state = entry->state;
    server->error = entry->error;
    // The preview picture, for good only (as serverFailed() reads the main file): gone from the server, not ours to
    // download there, or not the one announced (a preview is up before its LP goes: no upload still arriving). A
    // lost connection or a full quota keeps the sender's preview; Core loads the picture again (after a reconnect)
    // or on a click.
    const MediaError pe   = entry->previewError;
    server->previewFailed = entry->previewState == MediaState::Failed &&
                            (pe == MediaError::NotFound || pe == MediaError::Permission || pe == MediaError::Password || pe == MediaError::Mismatch);
}

bool kindFits(linkpreview::Kind announced, linkpreview::Kind link)
{
    using linkpreview::Kind;
    const auto picture = [](Kind k) { return k == Kind::Image || k == Kind::Gif; };
    if (announced == Kind::None || link == Kind::None)
        return false;
    return announced == link || (picture(announced) && picture(link));
}

bool lpFits(linkpreview::Kind announced, const linkpreview::Target& link)
{
    // An LP of a known kind under a page link would be a 2.4.0 trap (research: it downloads and never shows it):
    // LP belongs to the 2.4 classes only.
    return link.isLegacy() && kindFits(announced, link.kind);
}

bool lpxFits(linkpreview::Kind announced, const linkpreview::Target& link, bool webCards)
{
    using linkpreview::Kind;
    if (!link.isNewClass() || (link.cls == linkpreview::LinkClass::Web && !webCards))
        return false;
    // A share's card (design2 2.7) is a text card: only while cards for other web pages are on; its media always.
    if (announced == Kind::Card && !webCards && linkpreview::providers::cardsFollowWebRules(link.cls, link.provider))
        return false;
    return announced == Kind::Image || announced == Kind::Gif || announced == Kind::Video || announced == Kind::Loop || announced == Kind::Card;
}

linkcard::Waiting waitingShown(const Decision& d)
{
    switch (d.wait) {
    case Wait::Own:
        return linkcard::Waiting::Own;
    case Wait::Sender:
        return d.fromServer ? linkcard::Waiting::None : linkcard::Waiting::Sender;
    case Wait::Checking:
        return linkcard::Waiting::Checking;
    case Wait::Host:
        return linkcard::Waiting::Host;
    case Wait::Auto:
    case Wait::None:
        break;
    }
    return linkcard::Waiting::None;
}

bool serverUsable(const ServerState& server)
{
    if (!server.announced)
        return false;
    if (server.extended && server.kind == linkpreview::Kind::Card) {
        // Its text is a card (its picture comes from the server, or never). A card that is only a picture (no text)
        // whose picture can never come (gone, not ours to download there) is nothing: the receiver's own rules.
        return !(serverCardPictureGone(server) && server.card.description.isEmpty());
    }
    return !serverFailed(server) && !server.mainKey.isEmpty();
}

bool serverCardPictureGone(const ServerState& server)
{
    return server.extended && server.kind == linkpreview::Kind::Card && !server.mainKey.isEmpty() && server.state == MediaState::Failed
           && (server.error == MediaError::NotFound || server.error == MediaError::Permission || server.error == MediaError::Password);
}

linkpreview::CardData serverCardData(const ServerState& server)
{
    linkpreview::CardData data = server.card;
    data.sensitive             = data.sensitive || server.sensitive;
    if (server.mainKey.isEmpty() || serverCardPictureGone(server)) {
        // No picture, or one that can never come: a text card (never an empty box, never a play symbol on nothing).
        data.layout = linkpreview::CardLayout::None;
        data.play   = false;
    }
    return data;
}

bool serverFailed(const ServerState& server)
{
    if (!server.announced)
        return false;
    // The main file, for good: it is gone, or this viewer may not download it there. Anything else (a lost
    // connection, a full quota, a file that didn't match while the sender's upload was still arriving) keeps the
    // sender's preview with Core's own "Click to try again": a TeamSpeak hiccup never turns it into a card that asks
    // the website.
    if (server.state == MediaState::Failed)
        return server.error == MediaError::NotFound || server.error == MediaError::Permission || server.error == MediaError::Password;
    // An announced full file that isn't loaded automatically (a video, the data saver, a lower limit here): its
    // preview picture is all there is to show. Gone from the server, not ours to download there or not the one
    // announced (previewFailed, read by readEntry for good only): nothing is left.
    return server.hasMedia && server.previewFailed && server.state == MediaState::Idle;
}

Decision decide(linkpreview::Kind kind, bool hidden, const LocalState& local, const ServerState& server, Wait wait)
{
    Decision d;
    if (hidden) {
        d.look = linkcard::Look::Hidden;
        return d;
    }
    if (local.state == Local::Loaded) {
        if (kind == linkpreview::Kind::YouTube) {
            d.look = linkcard::Look::YouTube;
        } else if (!local.mediaKey.isEmpty()) {
            d.media    = true;
            d.mediaKey = local.mediaKey;
        }
        if (d.media || d.look == linkcard::Look::YouTube)
            return d;
    }
    if (server.announced && !serverFailed(server) && !server.mainKey.isEmpty()) {
        d.fromServer = true;
        if (kind == linkpreview::Kind::YouTube) {
            d.look = linkcard::Look::YouTube;
        } else if (kind == linkpreview::Kind::Video && !server.hasMedia) {
            d.look = linkcard::Look::Poster;
        } else {
            d.media    = true;
            d.mediaKey = server.mainKey;
        }
        return d;
    }
    switch (local.state) {
    case Local::Loading:
        d.look = linkcard::Look::Loading;
        break;
    case Local::Failed:
        d.look = linkcard::Look::Error;
        break;
    case Local::TooLarge:
        d.look = linkcard::Look::TooLarge;
        break;
    case Local::Paused:
        d.look = linkcard::Look::Click; // 2.6: waits for a click (or for the route); never an automatic load
        break;
    case Local::Idle:
    case Local::Loaded:
    case Local::OverAutoLimit:
        // Waiting for our pipeline or the sender: whatever size we learned elsewhere (their preview is small).
        // An automatic load: only a link nobody tried here yet (a file above the limit stays a click card).
        if (wait == Wait::Own || wait == Wait::Sender || wait == Wait::Checking || (wait == Wait::Auto && local.state == Local::Idle)) {
            d.look = linkcard::Look::Loading;
            d.wait = wait;
        } else {
            d.look = linkcard::Look::Click;
        }
        break;
    }
    return d;
}

Decision decide(const linkpreview::Target& target, bool hidden, const LocalState& local, const ServerState& server, Wait wait, const Context& context)
{
    using linkcard::Look;
    using linkpreview::Kind;
    Decision d;
    if (hidden) {
        d.look = d.underlying = Look::Hidden;
        return d;
    }
    const bool web = target.cls == linkpreview::LinkClass::Web;
    if (web && !context.webCards) {
        d.look = d.underlying = Look::Absent; // "Show cards for other web pages" off: never anything
        return d;
    }
    // 2. Loaded here.
    if (local.state == Local::Loaded) {
        if (target.cls == linkpreview::LinkClass::YouTube) {
            d.look = Look::YouTube;
        } else if (local.kind == Kind::Card && !local.card.isEmpty()) {
            // A share's card (2.7) shows only while cards for other web pages are on: nothing else to show then.
            if (!context.webCards && linkpreview::providers::cardsFollowWebRules(target.cls, target.provider)) {
                d.look = d.underlying = Look::Absent;
                return d;
            }
            d.look = Look::Embed;
        } else if (!local.mediaKey.isEmpty()) {
            d.media    = true;
            d.mediaKey = local.mediaKey;
            d.look     = Look::Loading; // only drawn while Core can't draw the media yet
        }
        if (d.media || d.look == Look::YouTube || d.look == Look::Embed) {
            d.underlying = d.look;
            return d;
        }
    }
    // 3. The sender's preview (LP for the 2.4 classes, LPX for the new ones: the chat checked which fits).
    if (serverUsable(server)) {
        d.fromServer = true;
        const Kind kind = server.extended ? server.kind : target.kind;
        if (server.extended && kind == Kind::Card) {
            d.look = Look::Embed;
        } else if (kind == Kind::YouTube) {
            d.look = Look::YouTube;
        } else if (kind == Kind::Video && !server.hasMedia) {
            d.look = Look::Poster;
        } else {
            d.media    = true;
            d.mediaKey = server.mainKey;
            // Drawn only while Core can't draw the media yet (an LPX's file is asked for once its slot is on screen).
            d.look = Look::Loading;
            d.wait = Wait::Sender;
        }
        d.underlying = d.look;
        return d;
    }
    // 4. Nothing to show here: no slot, except as the answer to a click (a click never seems to do nothing).
    if (local.state == Local::Nothing) {
        d.look    = local.clicked ? Look::Error : Look::Absent;
        d.nothing = local.clicked;
        d.underlying = d.look;
        return d;
    }
    const bool senderNothing = context.senderNothing && !local.clicked
                               && (local.state == Local::Idle || local.state == Local::OverAutoLimit || local.state == Local::Cooling);
    if (senderNothing) {
        d.look = d.underlying = Look::Absent; // the sender found nothing (or another website): nothing by itself
        return d;
    }
    // 5-7: strips (2.4.0's rules, and Cooling).
    switch (local.state) {
    case Local::Loading:
        d.look = Look::Loading;
        break;
    case Local::Failed:
        d.look = Look::Error;
        break;
    case Local::TooLarge:
        d.look = Look::TooLarge;
        break;
    case Local::Cooling:
        d.look = Look::Loading;
        d.wait = Wait::Host;
        break;
    case Local::Paused:
        d.look = Look::Click; // 2.6: waits for a click (or for the route); never an automatic load
        break;
    case Local::Idle:
    case Local::Loaded:
    case Local::OverAutoLimit:
    case Local::Nothing:
        if (wait == Wait::Own || wait == Wait::Sender || wait == Wait::Checking || (wait == Wait::Auto && local.state == Local::Idle)) {
            d.look = Look::Loading;
            d.wait = wait;
        } else {
            d.look = Look::Click;
        }
        break;
    }
    d.underlying = d.look;
    // The quiet class: a web page shows nothing while it waits, loads or fails by itself (Discord-like), and has no
    // click strip; a load the user started shows its strips. A web link that turned out to be media is a media slot.
    const bool mediaKnown = local.kind == Kind::Image || local.kind == Kind::Gif || local.kind == Kind::Video || local.kind == Kind::Loop;
    if (web && !mediaKnown && !(local.clicked && (local.state == Local::Loading || local.state == Local::Failed)))
        d.look = Look::Absent;
    return d;
}

// ---- 2.5: busy sites ------------------------------------------------------------------------------------------

LocalState afterBusy(const LocalState& before, qint64 retryAfterMs, bool soft, bool gateBusy, bool automatic, qint64 nowMs)
{
    LocalState l = before;
    l.progress   = -1.0;
    l.error.clear();
    if (!automatic) {
        // A click: the neutral busy line with Retry (a click never passes a cooldown).
        l.state = Local::Failed;
        l.error = errorText(QString::fromLatin1(linkpreview::errors::kRateLimited));
        return l;
    }
    const qint64 wait = retryAfterMs > 0 ? retryAfterMs : gateBusy ? kGateBusyRetryMs : 60000;
    const bool   hard = !soft && !gateBusy;
    if ((hard && before.coolRetries >= kMaxCoolRetries) || before.coolingMs + wait > kMaxCoolingMs) {
        l.state = Local::Failed; // asked often enough, or long enough: the busy line with Retry
        l.error = errorText(QString::fromLatin1(linkpreview::errors::kRateLimited));
        return l;
    }
    l.state       = Local::Cooling;
    l.retryAtMs   = nowMs + wait;
    l.coolRetries = before.coolRetries + (hard ? 1 : 0);
    l.coolingMs   = before.coolingMs + wait;
    l.softCooling = !hard;
    return l;
}

bool coolingDue(const LocalState& local, qint64 nowMs)
{
    return local.state == Local::Cooling && nowMs >= local.retryAtMs;
}

LocalState underGate(LocalState local, linkpreview::HostGate::Verdict gate, qint64 ms, qint64 nowMs)
{
    if (local.state != Local::Idle)
        return local;
    if (gate == linkpreview::HostGate::Verdict::Cooling && ms > 0) {
        local.state     = Local::Cooling;
        local.retryAtMs = nowMs + ms;
    } else if (gate == linkpreview::HostGate::Verdict::Walled) {
        // Like a load that met the wall (kWalled): nothing to show, the same for every link of that site; a click
        // (Load link previews) says "No preview for this page".
        local.state = Local::Nothing;
        local.error.clear();
    }
    return local;
}

LocalState clickedUnderGate(linkpreview::HostGate::Verdict gate)
{
    LocalState local;
    local.clicked = true;
    if (gate == linkpreview::HostGate::Verdict::Walled) {
        local.state = Local::Nothing; // decide(): "No preview for this page", Open in browser, no Retry
    } else {
        local.state = Local::Failed;
        local.error = errorText(QString::fromLatin1(linkpreview::errors::kRateLimited));
    }
    return local;
}

bool localFilesGone(const LocalState& local)
{
    if (local.state != Local::Loaded)
        return false;
    if (!local.thumbnailFile.isEmpty() && !QFileInfo::exists(local.thumbnailFile))
        return true;
    return !local.card.pictureFile.isEmpty() && !QFileInfo::exists(local.card.pictureFile);
}

bool crossSiteCardShown(const QString& finalHost, bool clicked, bool own)
{
    return finalHost.isEmpty() || clicked || own;
}

LocalState afterCard(LocalState local, const linkpreview::CardData& card, const QString& finalHost, bool own)
{
    local.state         = Local::Loaded;
    local.kind          = linkpreview::Kind::Card;
    local.card          = card;
    local.finalHost     = finalHost;
    local.heldCrossSite = false;
    if (card.isEmpty() || !crossSiteCardShown(finalHost, local.clicked, own)) {
        local.heldCrossSite = !card.isEmpty(); // in the cache: our own message's slot shows it (ownShowsHeldCard)
        local.state         = Local::Nothing;
        local.kind          = linkpreview::Kind::None;
        local.card          = linkpreview::CardData();
    }
    return local;
}

bool ownShowsHeldCard(const LocalState& local, bool own)
{
    return own && local.state == Local::Nothing && local.heldCrossSite;
}

linkpreview::CardData shownCardData(linkpreview::CardData data, linkpreview::LinkClass cls, const QString& host, bool crossSite,
                                    const QString& provider)
{
    data.title       = linkcard::cleanText(data.title, linkpreview::kCardTitleChars);
    data.author      = linkcard::cleanText(data.author, linkpreview::kCardAuthorChars);
    data.description = linkcard::cleanText(data.description, linkpreview::kCardDescriptionChars);
    const QString site = linkcard::cleanText(data.site, linkpreview::kCardSiteChars);
    const bool    web  = linkpreview::providers::cardsFollowWebRules(cls, provider); // 2.5: a share's card too
    data.site          = web && !crossSite && linkpreview::providers::siteNameMatches(site, host) ? site : QString();
    if (!web || crossSite)
        data.hasColor = false;
    return data;
}

bool stillOverLimit(const LocalState& local, qint64 cap)
{
    if (local.state != Local::OverAutoLimit)
        return false;
    const bool fits = cap > 0 && ((local.size > 0 && local.size <= cap) || (local.size < 0 && cap > local.capHit));
    return !fits;
}

const char* holdReasonName(HoldReason reason)
{
    switch (reason) {
    case HoldReason::None:
        return "none";
    case HoldReason::Hidden:
        return "hidden";
    case HoldReason::Setting:
        return "setting";
    case HoldReason::Waiting:
        return "waiting";
    case HoldReason::Offscreen:
        return "offscreen";
    case HoldReason::Turn:
        return "turn";
    case HoldReason::Budget:
        return "budget";
    case HoldReason::Gate:
        return "gate";
    case HoldReason::Cap:
        return "cap";
    case HoldReason::SenderNothing:
        return "sender-nothing";
    case HoldReason::Nothing:
        return "nothing";
    case HoldReason::Failed:
        return "failed";
    case HoldReason::Paused:
        return "paused";
    }
    return "none";
}

HoldReason holdReason(const Decision& d, const LocalState& local, const Context& context, bool onScreen, bool turnFree, bool budgetFree,
                      linkpreview::HostGate::Verdict gate, bool capOk)
{
    using linkcard::Look;
    using Verdict = linkpreview::HostGate::Verdict;
    if (d.look == Look::Hidden)
        return HoldReason::Hidden;
    if (!context.webCards && local.state == Local::Loaded && d.look == Look::Absent && d.underlying == Look::Absent)
        return HoldReason::Setting; // 2.5: a share's card loaded here while cards for other web pages are off
    if (d.media || d.fromServer || d.look == Look::YouTube || d.look == Look::Embed || d.look == Look::Poster || local.state == Local::Loaded)
        return HoldReason::None; // shown
    if (d.nothing || local.state == Local::Nothing)
        return HoldReason::Nothing;
    if (local.state == Local::Failed || local.state == Local::TooLarge)
        return HoldReason::Failed;
    if (local.state == Local::Paused)
        return HoldReason::Paused;
    if (local.state == Local::Loading)
        return HoldReason::None; // loading now
    if (context.senderNothing && d.look == Look::Absent)
        return HoldReason::SenderNothing;
    if (!context.webCards && d.look == Look::Absent && d.underlying == Look::Absent)
        return HoldReason::Setting;
    if (d.wait == Wait::Own || d.wait == Wait::Sender || d.wait == Wait::Checking)
        return HoldReason::Waiting;
    if (local.state == Local::Cooling || gate == Verdict::Cooling || gate == Verdict::Walled || gate == Verdict::Broken)
        return HoldReason::Gate;
    if (!context.autoOn)
        return HoldReason::Setting;
    if (!capOk || local.state == Local::OverAutoLimit)
        return HoldReason::Cap;
    if (!onScreen)
        return HoldReason::Offscreen;
    if (!turnFree)
        return HoldReason::Turn;
    if (!budgetFree)
        return HoldReason::Budget;
    return HoldReason::None;
}

// ---- 2.6 chat proxy ------------------------------------------------------------------------------------------------

bool viaChatProxy(proxy::Via via, proxy::Failure failure)
{
    if (via == proxy::Via::Bridge)
        return true;
    // Refused: the Windows route's refusals (W1, and a setup script that didn't answer in time, which only Windows mode
    // refuses with Slow) are the Windows proxy's; every other refusal is the chat proxy's (its password, its bridge).
    if (via == proxy::Via::Refused)
        return failure != proxy::Failure::SocksOnlyWindows && failure != proxy::Failure::UnsupportedWindowsProxy && failure != proxy::Failure::Slow;
    return false;
}

LocalState afterRouteFailure(LocalState local, const QString& code, proxy::Via via, proxy::Failure failure, bool decisive, quint64 generation,
                             bool chatProxyDown)
{
    local.code            = code;
    local.via             = via;
    local.proxyFailure    = failure;
    local.proxyDecisive   = decisive;
    local.routeGeneration = generation;
    local.progress        = -1.0;
    local.pause           = Pause::None;
    local.error.clear();
    const bool automatic = !local.clicked;
    if (code == QLatin1String(linkpreview::errors::kRouteDown)) {
        local.state = Local::Paused;
        local.pause = Pause::ChatProxy;
        local.via   = proxy::Via::Bridge;
    } else if (code == QLatin1String(linkpreview::errors::kProxy) && viaChatProxy(via, failure)) {
        if (automatic && decisive && chatProxyDown) {
            local.state = Local::Paused;
            local.pause = Pause::ChatProxy;
        } else {
            local.state = Local::Failed;
        }
    } else if (code == QLatin1String(linkpreview::errors::kProxy)) {
        if (automatic) {
            local.state = Local::Paused; // the quiet click look (F2): no error card for an automatic load
            local.pause = Pause::WindowsProxy;
        } else {
            local.state = Local::Failed;
        }
    } else {
        local.state = Local::Failed; // filtered (and anything else this is given)
    }
    return local;
}

linkcard::ProxyStrip proxyStripOf(const LocalState& local, bool proxySaved)
{
    using linkcard::ProxyStrip;
    if (local.state == Local::Paused) {
        if (local.pause == Pause::ChatProxy)
            return ProxyStrip::PausedChatProxy;
        if (local.pause == Pause::WindowsProxy)
            return ProxyStrip::PausedWindows;
        return ProxyStrip::None; // Click: the plain Load preview strip
    }
    if (local.state != Local::Failed)
        return ProxyStrip::None;
    if (local.code == QLatin1String(linkpreview::errors::kFiltered))
        return proxySaved ? ProxyStrip::FilteredUseProxy : ProxyStrip::FilteredSetUp;
    if (local.code == QLatin1String(linkpreview::errors::kProxy) || local.code == QLatin1String(linkpreview::errors::kRouteDown))
        return viaChatProxy(local.via, local.proxyFailure) ? ProxyStrip::ChatProxy : ProxyStrip::Windows;
    return ProxyStrip::None;
}

bool pausesOnDown(const LocalState& local, quint64 generation)
{
    return local.state == Local::Failed && !local.clicked && local.code == QLatin1String(linkpreview::errors::kProxy) && local.proxyDecisive
           && viaChatProxy(local.via, local.proxyFailure) && local.routeGeneration == generation;
}

bool resumesOnHealth(const LocalState& local)
{
    return local.state == Local::Paused && local.pause == Pause::ChatProxy;
}

LocalState pausedForChatProxy(quint64 generation)
{
    LocalState loading;
    loading.state = Local::Loading;
    return afterRouteFailure(loading, QString::fromLatin1(linkpreview::errors::kRouteDown), proxy::Via::Unset, proxy::Failure::None, false, generation,
                             true);
}

bool pausesForChatProxy(bool chatProxyDown, bool cacheChecked, bool ownPipeline)
{
    return chatProxyDown && cacheChecked && !ownPipeline;
}

AfterRoute afterRouteChange(const LocalState& local, bool toChatProxy)
{
    const bool proxyFailed = local.state == Local::Failed
                             && (local.code == QLatin1String(linkpreview::errors::kProxy) || local.code == QLatin1String(linkpreview::errors::kRouteDown));
    const bool filtered    = local.state == Local::Failed && local.code == QLatin1String(linkpreview::errors::kFiltered);
    const bool walled      = local.state == Local::Nothing && local.code == QLatin1String(linkpreview::errors::kWalled);
    const bool paused      = local.state == Local::Paused;
    if (toChatProxy)
        return paused || proxyFailed || filtered || walled ? AfterRoute::Idle : AfterRoute::Keep;
    // Away from a proxy (custom -> Windows or direct, Windows <-> direct): nothing that was posted while the user relied on
    // a proxy is fetched outside it without a click (S4).
    if ((paused || proxyFailed || walled) && !local.clicked)
        return AfterRoute::Click;
    return AfterRoute::Keep;
}

AfterRoute afterRouteChangeResumed(const LocalState& local, bool toChatProxy)
{
    if (toChatProxy || !resumedStillWaits(local))
        return AfterRoute::Keep;
    // Review fix: Cooling (a site's 429, the gate's wait) hasn't loaded it: it would load by itself on the new route once
    // its time comes. A link that waits for a click already stays as it is (counted once).
    return local.state == Local::Paused && local.pause == Pause::Click ? AfterRoute::Keep : AfterRoute::Click;
}

bool resumedStillWaits(const LocalState& local)
{
    if (local.clicked)
        return false; // the user's now
    return local.state == Local::Idle || local.state == Local::Loading || local.state == Local::Cooling || local.state == Local::Paused;
}

int routeChangePass(QHash<QString, LocalState>* local, QSet<QString>* resumed, const QSet<QString>& pipelineWaits, bool toChatProxy,
                    QStringList* idled)
{
    LocalState click;
    click.state = Local::Paused;
    click.pause = Pause::Click;
    int waiting = 0;
    for (auto it = local->begin(); it != local->end();) {
        const AfterRoute next = afterRouteChange(it.value(), toChatProxy);
        if (next == AfterRoute::Idle) {
            // Back to the normal rules on the chat proxy; it still waited for a proxy until it loads (review fix).
            resumed->insert(it.key());
            if (idled)
                idled->append(it.key());
            it = local->erase(it);
            continue;
        }
        if (next == AfterRoute::Click) {
            it.value() = click;
            ++waiting;
        }
        ++it;
    }
    for (auto it = resumed->begin(); it != resumed->end();) {
        // Review fix: our own pipeline's download of the link is its own (as for the running loads): never canceled or
        // turned into a click here; its end decides what the slot shows (our own message's error with Retry included).
        if (pipelineWaits.contains(*it)) {
            ++it;
            continue;
        }
        const LocalState l = local->value(*it);
        if (toChatProxy) {
            if (resumedStillWaits(l))
                ++it; // still remembered (review fix: custom -> custom kept them only until the next pass)
            else
                it = resumed->erase(it);
            continue;
        }
        if (afterRouteChangeResumed(l, false) == AfterRoute::Click) {
            local->insert(*it, click);
            ++waiting;
        }
        it = resumed->erase(it); // a click, its end, or Paused: afterRouteChange's rules from now on
    }
    return waiting;
}

ProxyMenu proxyMenuOf(const LocalState& local, bool proxyOn, bool proxySaved)
{
    ProxyMenu        m;
    const linkcard::ProxyStrip strip = proxyStripOf(local, proxySaved);
    const bool onChat   = strip == linkcard::ProxyStrip::ChatProxy || strip == linkcard::ProxyStrip::PausedChatProxy;
    const bool windows  = strip == linkcard::ProxyStrip::Windows || strip == linkcard::ProxyStrip::PausedWindows;
    const bool filtered = strip == linkcard::ProxyStrip::FilteredUseProxy || strip == linkcard::ProxyStrip::FilteredSetUp;
    m.turnOff  = onChat && proxyOn;
    m.turnOn   = (filtered || windows) && proxySaved && !proxyOn;
    m.settings = onChat || windows || filtered || local.state == Local::Paused;
    return m;
}

// ---- Core links ---------------------------------------------------------------------------------------

linkpreview::Announcement withFileBrowser(linkpreview::Announcement a, const QString& serverUid, quint64 channelId)
{
    if (a.serverUid.isEmpty())
        a.serverUid = serverUid;
    if (a.channelId == 0)
        a.channelId = channelId;
    return a;
}

bool serverLink(const linkpreview::Announcement& a, MediaLink* main)
{
    if (!main || a.serverUid.isEmpty() || a.channelId == 0 || !linkpreview::isValidAny(a) || a.previewPath.isEmpty())
        return false; // nothing that can be checked: nothing is downloaded
    const MediaLink link = linkpreview::linkFor(a);
    if (!link.isValid() || link.sha256.size() != 32)
        return false;
    *main = link;
    return true;
}

QUrl webAddress(const QString& href, const linkpreview::Target& target)
{
    // Only an href that classify() takes as this very link (its rules: no user info, standard ports, a public host).
    // 2.5: read the same way the chat found it (trimLinkEnd); http links of every class open as https.
    const QString link = linkpreview::trimLinkEnd(href);
    if (!link.isEmpty() && target.isValid() && linkpreview::classify(link).key == target.key) {
        QUrl url(link, QUrl::TolerantMode);
        if (url.scheme() == QLatin1String("http"))
            url.setScheme(QString::fromLatin1("https"));
        url.setPort(-1); // the standard one (classify takes no other)
        if (url.isValid() && url.scheme() == QLatin1String("https"))
            return url;
    }
    return target.isNewClass() && target.page.isValid() ? target.page : target.url;
}

QString errorText(const QString& code)
{
    // The website's details (blocked, offline, timeout, tls, ...) go to the private log only: one neutral line
    // for all of them. Only what the user can act on is said.
    if (code == QLatin1String(linkpreview::errors::kUnavailable))
        return i18n::t("This video isn't available");
    if (code == QLatin1String(linkpreview::errors::kNotFound))
        return i18n::t("The file isn't on the website any more");
    if (code == QLatin1String(linkpreview::errors::kBadType))
        return i18n::t("This link isn't a picture or video");
    if (code == QLatin1String(linkpreview::errors::kTooManyPixels))
        return i18n::t("The picture is too large to show");
    // 2.5
    if (code == QLatin1String(linkpreview::errors::kRateLimited) || code == QLatin1String(linkpreview::errors::kGateBusy))
        return i18n::t("The site is busy right now. Try again later.");
    if (code == QLatin1String(linkpreview::errors::kUnsupported))
        return i18n::t("This picture can't be shown here");
    return {};
}

bool isNothingError(const QString& code)
{
    return code == QLatin1String(linkpreview::errors::kNothing) || code == QLatin1String(linkpreview::errors::kWalled)
           || code == QLatin1String(linkpreview::errors::kExpired) || code == QLatin1String(linkpreview::errors::kWebOff);
}

bool loadFollowsWebCards(const linkpreview::Target& target)
{
    // Every switch-dependent step of a load is the share plan's (Fetcher sharePage: a redirect to another host, an
    // unconfirmed page); a Web link's load never starts with the switch off and stops when it goes off (R21).
    return linkpreview::providers::cardsFollowWebRules(target.cls, target.provider) && target.cls != linkpreview::LinkClass::Web;
}

bool restartForWebCards(const LocalState& local, const linkpreview::Target& target, bool webCards)
{
    return local.state == Local::Loading && local.webCards != webCards && loadFollowsWebCards(target);
}

QString webServerUid()
{
    return QString::fromLatin1("tsmedia-web");
}

MediaLink webLink(const linkpreview::Target& target, const QString& mime, qint64 size, int width, int height, qint64 durationMs, bool loop, bool sensitive)
{
    MediaLink link;
    link.serverUid = webServerUid();
    link.channelId = 1;
    link.path      = QString::fromLatin1("/web/") + target.key;
    link.protocol  = MediaLink::kProtocol;
    link.size      = size > 0 ? static_cast<quint64>(size) : 0;
    link.width     = boundedSide(width);
    link.height    = boundedSide(height);
    link.durationMs = durationMs > 0 ? durationMs : 0;
    // The name for Save as and the viewer's title: the address' file name, letters, digits and . _ - only.
    QString base = QFileInfo(target.url.path()).completeBaseName();
    QString clean;
    for (const QChar c : base) {
        if (clean.size() >= 60)
            break;
        clean += (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')) && c.unicode() < 0x2000 ? c : QLatin1Char('_');
    }
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    if (clean.isEmpty())
        clean = target.kind == linkpreview::Kind::Video || target.kind == linkpreview::Kind::Loop ? QString::fromLatin1("video") : QString::fromLatin1("image");
    link.fileName = clean + QLatin1Char('.') + extensionFor(mime, target);
    link.loop     = loop;      // 2.5: played like a GIF (never written into a link)
    link.spoiler  = sensitive; // 2.5: the page says it is for adults: the spoiler cover
    return link;
}

bool isWebLink(const MediaLink& link)
{
    return link.serverUid == webServerUid();
}

} // namespace linkstate
