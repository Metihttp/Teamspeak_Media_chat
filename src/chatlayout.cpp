#include "chatlayout.h"

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QImageReader>
#include <QRunnable>
#include <QThreadPool>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

#include <algorithm>
#include <climits>
#include <functional>

#include "actionbar.h"
#include "chatemoji.h"
#include "chatintegration.h"
#include "chatreactions.h"
#include "chatreplies.h"
#include "chatscroll.h"
#include "emojirender.h" // pictures the worker drew (emoji in names)
#include "emojitext.h"   // emoji in names still on their way
#include "i18n.h"
#include "replydoc.h"
#include "settings.h"
#include "ts3api.h"
#include "uiutil.h"

namespace {

using layoutart::Mode;
using layoutdoc::RowType;

constexpr int    kStepMs        = 4;   // work per step
constexpr int    kStepEveryMs   = 16;
constexpr int    kPaintBudgetMs = 8;   // pictures drawn right before one paint
constexpr int    kMaxRendered   = 40;  // pictures kept per chat before those off screen go back to blank
constexpr int    kMaxArrivals   = 512;
constexpr int    kMaxBurst      = 64;  // lines appended in one turn of the event loop, at most (more: history)
constexpr int    kMinBarWidth   = 240; // narrower chats get no action bar
constexpr int    kBarRight      = 12;
constexpr int    kResizeMs      = 180; // a new chat width is applied once resizing pauses

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

// Where Qt draws an inline picture of ours on its line (relative to the block's layout), by its alignment:
// AlignBottom (compact heads) on the line's bottom, AlignMiddle (cozy system rows) around the middle of the
// x-height, anything else with its bottom on the baseline.
qreal pictureTop(const QTextLine& line, const QTextImageFormat& image)
{
    switch (image.verticalAlignment()) {
    case QTextCharFormat::AlignBottom:
        return line.y() + line.height() - image.height();
    case QTextCharFormat::AlignMiddle: {
        const QFontMetricsF fm(image.font());
        return line.y() + line.ascent() - (image.height() + fm.xHeight() / 2.0) / 2.0;
    }
    default:
        return line.y() + line.ascent() - image.height();
    }
}

bool isBodyLink(const QTextCharFormat& f)
{
    if (!f.isAnchor() || f.isImageFormat())
        return false;
    const QString href = f.anchorHref();
    return !layoutformat::isClientHref(href) && !href.contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

// A mouse-transparent layer over the chat for skins that paint over what we put under the text.
class Overlay : public QWidget
{
  public:
    Overlay(QWidget* viewport, std::function<void(QPainter&, const QRegion&)> paint)
        : QWidget(viewport)
        , m_paint(std::move(paint))
    {
        setObjectName(QString::fromLatin1("tsmediaLayoutOverlay")); // swept at shutdown with our other widgets
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(viewport->rect());
        show();
    }

  protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter p(this);
        if (m_paint)
            m_paint(p, event->region());
    }

  private:
    std::function<void(QPainter&, const QRegion&)> m_paint;
};

} // namespace

// ============================================================================================
// Life
// ============================================================================================

ChatLayout::ChatLayout(ChatIntegration* chat, Core* core)
    : QObject(chat)
    , m_chat(chat)
    , m_core(core)
{
    m_tag = QString::number(QRandomGenerator::global()->generate(), 16);
    const Settings& s = Settings::instance();
    m_mode            = static_cast<Mode>(qBound(0, s.chatLayout, 2));
    m_group           = s.chatGroupMessages;
    m_introDone       = s.chatLayoutIntroShown;

    // Emoji in names drawn as text until their pictures arrive: drawn again then.
    if (emoji::ImageNotifier* n = emoji::notifier()) {
        connect(n, &emoji::ImageNotifier::imagesReady, this, [this] {
            for (auto v = m_views.begin(); v != m_views.end(); ++v) {
                ++v->renderStamp;
                if (v->browser && v->browser->isVisible() && m_mode != Mode::Classic)
                    v->browser->viewport()->update();
            }
        });
    }

    // Owned timers only: they stop with us while the DLL is still loaded.
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ChatLayout::step);

    // "Today" and "Yesterday" move on at midnight.
    m_midnight = new QTimer(this);
    m_midnight->setSingleShot(true);
    connect(m_midnight, &QTimer::timeout, this, [this] {
        for (auto it = m_views.begin(); it != m_views.end(); ++it)
            refreshEnv(it.value());
        schedule(0);
        const QDateTime now      = QDateTime::currentDateTime();
        const QDateTime midnight = QDateTime(now.date().addDays(1), QTime(0, 0, 1));
        m_midnight->start(static_cast<int>(qBound<qint64>(1000, now.msecsTo(midnight), 24LL * 3600 * 1000)));
    });
    {
        const QDateTime now      = QDateTime::currentDateTime();
        const QDateTime midnight = QDateTime(now.date().addDays(1), QTime(0, 0, 1));
        m_midnight->start(static_cast<int>(qBound<qint64>(1000, now.msecsTo(midnight), 24LL * 3600 * 1000)));
    }

    // Avatars (P2): one worker, owned (waited for before we go).
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(1);
    m_pool->setExpiryTimeout(5000);

    m_resize = new QTimer(this);
    m_resize->setSingleShot(true);
    m_resize->setInterval(kResizeMs);
    connect(m_resize, &QTimer::timeout, this, [this] {
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            if (it->pendingWidth > 0) {
                it->pendingWidth = 0;
                it->styleKey.clear(); // the new width counts now
                refreshEnv(it.value());
            }
        }
        schedule(0);
    });
}

ChatLayout::~ChatLayout()
{
    // No worker of ours runs past this point (its results are posted to us and dropped with us).
    m_pool->clear();
    m_pool->waitForDone();
    m_timer->stop();
    m_midnight->stop();
    m_resize->stop();
    if (m_bar)
        delete m_bar.data();
    // The chats stay with TeamSpeak: every block back as TeamSpeak made it (one edit per chat).
    QSet<QTextDocument*> shown;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        View& v = it.value();
        if (v.overlayWidget)
            delete v.overlayWidget.data();
        if (!v.browser)
            continue;
        v.browser->removeEventFilter(this);
        shown.insert(v.browser->document());
        restoreView(v, true);
    }
    for (const QPointer<QTextDocument>& doc : qAsConst(m_retired)) {
        if (doc && !shown.contains(doc.data())) {
            layoutformat::MutatingScope mutating;
            layoutdoc::giveBack(doc.data());
        }
    }
}

bool ChatLayout::isMutating()
{
    return layoutformat::mutating();
}

layoutart::Mode ChatLayout::mode() const
{
    return m_mode;
}

ChatLayout::View* ChatLayout::viewFor(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

ChatLayout::View* ChatLayout::viewForViewport(QObject* viewport)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser && it->browser->viewport() == viewport)
            return &it.value();
    }
    return nullptr;
}

int ChatLayout::indentFor(QTextBrowser* browser) const
{
    if (m_mode == Mode::Classic)
        return 0;
    auto it = m_views.constFind(browser);
    if (it != m_views.constEnd() && it->env.tokens.G > 0)
        return m_mode == Mode::Compact ? it->env.tokens.L : it->env.tokens.G;
    const layoutart::Tokens t = layoutart::tokensFor(m_mode, browser ? browser->document()->defaultFont() : QFont(), false);
    return t.indent();
}

ActionBar* ChatLayout::bar() const
{
    return m_bar.data();
}

bool ChatLayout::overlayMode(QTextBrowser* browser) const
{
    auto it = m_views.constFind(browser);
    return it != m_views.constEnd() && it->overlay;
}

void ChatLayout::attach(QTextBrowser* browser)
{
    if (!browser || m_views.contains(browser))
        return;
    View v;
    v.browser  = browser;
    v.document = browser->document();
    m_views.insert(browser, v);
    View& view = m_views[browser];
    connect(browser, &QObject::destroyed, this, [this, browser] {
        if (m_barIn == browser)
            hideBar(false);
        m_views.remove(browser);
    });
    browser->installEventFilter(this); // keyboard context menus
    const QPointer<QTextBrowser> guard(browser);
    connect(browser->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, guard] {
        View* v = guard ? viewFor(guard.data()) : nullptr;
        if (!v)
            return;
        if (v->overlayWidget)
            v->overlayWidget->setGeometry(guard->viewport()->rect()); // QWidget::scroll moved it
        // The chat moved under a still pointer: its row is found again (after QTextBrowser's own handling).
        QTimer::singleShot(0, this, [this, guard] {
            View* w = guard ? viewFor(guard.data()) : nullptr;
            if (w && guard->viewport()->underMouse())
                updateHover(*w, guard->viewport()->mapFromGlobal(QCursor::pos()), false);
            else if (w && w->hover >= 0 && !(m_bar && m_bar->underMouse()))
                clearHover(*w);
        });
    });
    watch(view);
    reset(view);
    schedule(0);
}

void ChatLayout::documentSwapped(QTextBrowser* browser)
{
    View* v = viewFor(browser);
    if (!v || v->document == browser->document())
        return;
    // TeamSpeak may show the old document again later: given back on unload (and its pictures dropped).
    if (v->document) {
        disconnect(v->document.data(), &QTextDocument::contentsChange, this, nullptr);
        if (!m_retired.contains(v->document))
            m_retired.append(v->document);
    }
    m_retired.removeAll(QPointer<QTextDocument>());
    m_retired.removeAll(QPointer<QTextDocument>(browser->document()));
    v->document = browser->document();
    if (m_barIn == browser)
        hideBar(false);
    watch(*v);
    reset(*v);
    schedule(0);
}

void ChatLayout::reset(View& v)
{
    v.tagEpoch.clear();
    v.tagBlock.clear();
    v.arrivedByTag.clear();
    v.arrivals.clear();
    v.burst.clear();
    v.dirty.clear();
    v.restyle.clear();
    v.rendered.clear();
    v.dates.clear();
    v.datesValid = false;
    v.datesFrom  = INT_MAX;
    v.hover      = -1;
    v.linkRects.clear();
    v.styleKey.clear();
    v.statsValid = false;
    ++v.epoch;
    v.historyNext = v.document ? v.document->blockCount() - 1 : -1;
    v.blocksSeen  = v.document ? v.document->blockCount() : 0;
}

void ChatLayout::dropFirst(View& v, int dropped)
{
    // Block numbers of what is left: n becomes n - dropped; what pointed into the lines that went is gone.
    const auto moved = [dropped](int n) { return n < 0 ? n : (n >= dropped ? n - dropped : -1); };
    for (auto it = v.tagBlock.begin(); it != v.tagBlock.end();) {
        const int n = moved(it.value());
        if (n < 0) {
            it = v.tagBlock.erase(it);
        } else {
            it.value() = n;
            ++it;
        }
    }
    QSet<int> restyle;
    for (const int n : qAsConst(v.restyle)) {
        if (moved(n) >= 0)
            restyle.insert(moved(n));
    }
    v.restyle = restyle;
    // The days of the lines left stay theirs (the day line above them may have gone with the first lines).
    if (v.datesValid && dropped <= v.dates.size()) {
        v.dates.remove(0, dropped);
        if (v.datesFrom != INT_MAX)
            v.datesFrom = qMax(0, v.datesFrom - dropped);
    } else {
        v.datesValid = false;
    }
    // Arrivals in the lines that went now point at the first line left.
    while (!v.arrivals.isEmpty() && v.arrivals.first().first.blockNumber() == 0)
        v.arrivals.removeFirst();
    if (v.historyNext >= 0)
        v.historyNext = moved(v.historyNext);
    v.hover           = moved(v.hover);
    if (v.hover < 0)
        v.hoverSlot = QRect();
    v.chipHover       = moved(v.chipHover);
    v.replyPressBlock = moved(v.replyPressBlock);
    if (v.replyPressBlock < 0)
        v.replyPressed = false;
    if (m_barIn == v.browser && m_barBlock >= 0) {
        m_barBlock = moved(m_barBlock);
        if (m_barBlock < 0)
            hideBar(false);
    }
    v.renderedShown = {-1, -1};
}

void ChatLayout::watch(View& view)
{
    QTextDocument* doc = view.document;
    if (!doc)
        return;
    const QPointer<QTextBrowser> browser = view.browser;
    connect(doc, &QTextDocument::contentsChange, this, [this, doc, browser](int position, int removed, int added) {
        View* v = browser ? viewFor(browser.data()) : nullptr;
        if (!v || v->document != doc)
            return;
        // Blocks are only added or removed by TeamSpeak (no module of ours does): how many went since.
        const int blocks  = doc->blockCount();
        const int dropped = v->blocksSeen - blocks;
        v->blocksSeen     = blocks;
        if (layoutformat::mutating() || ChatEmoji::isMutating() || m_chat->m_mutating)
            return; // our own edits, or another module's (they tell us what we need)
        if (removed > 0 && doc->characterCount() <= 1) {
            reset(*v); // TeamSpeak cleared the chat (its resources went with it)
            return;
        }
        const int end = qMax(0, doc->characterCount() - 1);
        if (removed > 0 && position <= 1 && dropped > 0) {
            // TeamSpeak's line limit (QTextDocument::maximumBlockCount): the first lines went, with every new
            // line once the chat is full. What we keep by block number moves down with the rest.
            dropFirst(*v, dropped);
        } else {
            // Days: lines appended at the end are read on their own; anything else reads every line again.
            if (removed == 0 && added > 0 && position + added >= end - 1)
                v->datesFrom = qMin(v->datesFrom, doc->findBlock(qBound(0, position, end)).blockNumber());
            else
                v->datesValid = false;
            if (removed > 0 && position <= 1) {
                // Lines dropped at the top, but not as whole blocks: block numbers may have moved.
                v->tagBlock.clear();
                v->restyle.clear();
            }
        }
        const int last = qMax(0, doc->characterCount() - 1);
        QTextCursor from(doc);
        from.setPosition(qBound(0, position, last));
        from.setKeepPositionOnInsert(true);
        QTextCursor to(doc);
        to.setPosition(qBound(0, position + added, last));
        if (v->dirty.size() > 64)
            v->historyNext = doc->blockCount() - 1; // a lot at once: everything again (what is done is skipped)
        else
            v->dirty.append({from, to});
        // Appended at the end while we watch: when it came (grouping without timestamps, "today"). Counted once
        // this turn of the event loop is over (settleBurst): TeamSpeak's history comes many lines in one go,
        // also when a reconnect fills the cleared chat again one line at a time.
        if (added > 0 && position + added >= last - 1 && v->burst.size() <= kMaxBurst) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (QTextBlock b = doc->findBlock(qBound(0, position, last)); b.isValid() && v->burst.size() <= kMaxBurst; b = b.next())
                v->burst.append({QTextCursor(b), now});
        }
        // Before TeamSpeak paints the new lines (its paint request is posted with a low priority).
        if (!m_quickPosted) {
            m_quickPosted = true;
            QMetaObject::invokeMethod(this, [this] { quickStep(); }, Qt::QueuedConnection);
        }
    });
}

// ============================================================================================
// Settings and the environment
// ============================================================================================

void ChatLayout::settingsChanged()
{
    const Settings& s     = Settings::instance();
    const Mode      mode  = static_cast<Mode>(qBound(0, s.chatLayout, 2));
    const bool      group = s.chatGroupMessages;
    if (!s.chatHoverActions)
        hideBar(false);
    if (mode == m_mode && group == m_group) {
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            if (it->browser)
                it->browser->viewport()->update();
        }
        schedule(0); // mentions or collapsed events switched: the style key tells, in steps
        return;
    }
    // A switch gives everything back, then the new look comes in steps (the screen first).
    hideBar(false);
    m_mode  = mode;
    m_group = group;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        View& v = it.value();
        if (!v.browser)
            continue;
        restoreView(v, false);
        if (v.overlayWidget)
            delete v.overlayWidget.data();
        v.overlay = false;
        v.checked = false;
        reset(v);
    }
    schedule(0);
}

layoutdoc::Env ChatLayout::envFor(View& v) const
{
    layoutdoc::Env env;
    QTextBrowser*  b   = v.browser;
    QTextDocument* doc = b->document();
    const PreviewStyle style = m_chat->styleFor(b);
    env.mode   = m_mode;
    env.group  = m_group;
    env.tag    = m_tag;
    env.today  = QDate::currentDate();
    env.dpr    = b->devicePixelRatioF();
    // What the chat's lines say about the skin (the usual nickname and link colours, AM/PM): read from a few
    // hundred blocks, so only now and then (every step would cost milliseconds).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!v.statsValid || (doc->blockCount() != v.statsBlocks && now - v.statsMs > 2000)) {
        v.statNick    = layoutdoc::usualNickColor(doc);
        v.statLink    = layoutdoc::usualLinkColor(doc);
        v.statAmPm    = layoutdoc::usesAmPm(doc);
        v.statsValid  = true;
        v.statsBlocks = doc->blockCount();
        v.statsMs     = now;
    }
    env.tokens = layoutart::tokensFor(m_mode == Mode::Classic ? Mode::Cozy : m_mode, doc->defaultFont(), v.statAmPm);
    QColor base = b->viewport()->palette().color(QPalette::Base);
    if ((base.lightness() < 128) != style.dark)
        base = QColor(); // a style sheet colours the chat: the theme's default
    const QColor link = v.statLink;
    env.colors        = layoutart::colorsFor(style.dark, base, b->palette().color(QPalette::Text), link.isValid() ? link : b->palette().color(QPalette::Link));
    const int margin  = static_cast<int>(2 * doc->documentMargin());
    env.contentWidth  = qMax(120, m_chat->previewAreaWidth(b) - margin);
    env.usualNickColor = v.statNick;
    // Mentions (P2): a shown chat belongs to the current server tab; what it learned there stays with it.
    env.mentions = Settings::instance().chatMentions;
    if (b->isVisible()) {
        const uint64 sch = ts3::currentConnection();
        if (sch && ts3::isConnected(sch)) {
            char* nick = nullptr;
            if (ts3::funcs.getClientSelfVariableAsString && ts3::funcs.getClientSelfVariableAsString(sch, CLIENT_NICKNAME, &nick) == ERROR_ok) {
                const QString own = ts3::takeString(nick);
                if (!own.isEmpty() && own.size() <= 64) {
                    v.ownNick = own;
                    v.ownUid  = ts3::ownUid(sch);
                }
            }
        }
    }
    if (b->isVisible()) {
        const uint64 sch = ts3::currentConnection();
        if (sch && ts3::isConnected(sch)) {
            const QString server = ts3::serverUid(sch);
            if (!server.isEmpty() && server.size() <= 128)
                v.serverUid = server;
        }
    }
    env.ownNick                  = v.ownNick;
    env.ownUid                   = v.ownUid;
    env.collapse                 = Settings::instance().chatCollapseEvents;
    env.expandedRuns             = v.expandedRuns;
    const QPointer<QTextBrowser> guard(b);
    ChatIntegration*             chat = m_chat;
    env.replyHeader                   = [chat, guard](int block, replyart::Header* header) {
        return guard && chat->m_replies && chat->m_replies->replyHeader(guard.data(), block, header);
    };
    return env;
}

QString ChatLayout::styleKeyOf(const layoutdoc::Env& env) const
{
    const layoutart::Colors& c = env.colors;
    return QStringList{QString::number(static_cast<int>(env.mode)), QString::number(env.group), env.tokens.body.toString(), QString::number(env.tokens.ampm),
                       c.base.name(), c.text.name(), c.link.name(), QString::number(c.dark), QString::number(env.dpr), QString::number(env.contentWidth),
                       env.today.toString(Qt::ISODate), QString::number(env.mentions), env.ownNick, env.ownUid, QString::number(env.collapse)}
        .join(QLatin1Char('|'));
}

void ChatLayout::refreshEnv(View& v)
{
    if (!v.browser)
        return;
    layoutdoc::Env env = envFor(v);
    const QString  key = styleKeyOf(env);
    if (key == v.styleKey)
        return;
    if (!v.styleKey.isEmpty()) {
        // Only the width: applied once resizing pauses (a drag of the window's edge would restyle each pixel).
        layoutdoc::Env same = env;
        same.contentWidth   = v.env.contentWidth;
        if (styleKeyOf(same) == v.styleKey) {
            if (v.pendingWidth != env.contentWidth) {
                v.pendingWidth = env.contentWidth;
                m_resize->start();
            }
            return;
        }
    }
    v.env      = env;
    v.styleKey = key;
    ++v.renderStamp;
    ++v.epoch; // everything again, in steps (the screen first)
    v.historyNext = v.document ? v.document->blockCount() - 1 : -1;
    v.datesValid  = false;
    if (m_bar && m_barIn == v.browser)
        m_bar->setLook(v.env.colors, m_mode == Mode::Compact);
}

QDate ChatLayout::dateFor(View& v, int n)
{
    if (!v.datesValid) {
        v.dates      = layoutdoc::datesOf(v.document, v.env.today);
        v.datesValid = true;
        v.datesFrom  = INT_MAX;
    } else if (v.datesFrom < v.dates.size() || v.dates.size() != v.document->blockCount()) {
        layoutdoc::extendDates(v.document, v.env.today, &v.dates, qMin(v.datesFrom, v.dates.size())); // only the new lines
        v.datesFrom = INT_MAX;
    }
    QDate d = n >= 0 && n < v.dates.size() ? v.dates.at(n) : QDate();
    if (!d.isValid()) {
        // Before any day line: today for lines that arrived while we watched.
        const QTextBlock b   = v.document->findBlockByNumber(n);
        const qint64     ms  = arrivedFor(v, n, b.blockFormat().intProperty(layoutformat::kBlockTag));
        if (ms > 0)
            d = QDateTime::fromMSecsSinceEpoch(ms).date();
    }
    return d;
}

void ChatLayout::settleBurst(View& v)
{
    if (v.burst.isEmpty())
        return;
    QVector<QPair<QTextCursor, qint64>> burst;
    burst.swap(v.burst);
    if (burst.size() > kMaxBurst)
        return; // history being loaded
    QVector<QTextBlock> lines;
    lines.reserve(burst.size());
    for (const auto& entry : qAsConst(burst))
        lines.append(entry.first.block());
    const qint64 when = burst.first().second;
    for (const int n : layoutdoc::arrivedNow(lines, QTime::currentTime().msecsSinceStartOfDay() / 1000)) {
        const QTextBlock b = v.document ? v.document->findBlockByNumber(n) : QTextBlock();
        if (b.isValid())
            v.arrivals.append({QTextCursor(b), when});
    }
    while (v.arrivals.size() > kMaxArrivals)
        v.arrivals.removeFirst();
}

qint64 ChatLayout::arrivedFor(View& v, int n, int tag)
{
    settleBurst(v); // the lines of a turn that is over
    if (tag > 0) {
        const auto it = v.arrivedByTag.constFind(tag);
        if (it != v.arrivedByTag.constEnd())
            return it.value();
    }
    // Arrivals are appended at the document's end, so their blocks only grow along the list: a binary search
    // (a lookup per block for every line of a long history otherwise walks all 512).
    int lo = 0;
    int hi = v.arrivals.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (v.arrivals.at(mid).first.blockNumber() <= n)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0 && v.arrivals.at(lo - 1).first.blockNumber() == n)
        return v.arrivals.at(lo - 1).second; // the newest for n
    return 0;
}

// ============================================================================================
// The steps
// ============================================================================================

void ChatLayout::schedule(int delayMs)
{
    if (!m_timer->isActive() || delayMs < m_timer->remainingTime())
        m_timer->start(delayMs);
}

void ChatLayout::quickStep()
{
    m_quickPosted = false;
    step();
}

void ChatLayout::afterScan(QTextBrowser* browser)
{
    View* v = viewFor(browser);
    if (!v)
        return;
    ++v->renderStamp; // reply lines and previews may have changed what heads show
    QElapsedTimer clock;
    clock.start();
    if (stepView(*v, kStepMs, false))
        schedule(kStepEveryMs);
}

void ChatLayout::processNow(QTextBrowser* browser)
{
    View* v = viewFor(browser);
    if (!v)
        return;
    while (stepView(*v, -1, true)) {
    }
}

void ChatLayout::step()
{
    QElapsedTimer clock;
    clock.start();
    QVector<View*> order;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        settleBurst(it.value()); // every chat, in any look: the lines of the turn that is over
        if (it->browser)
            order.append(&it.value());
    }
    // Chats on screen first.
    std::stable_sort(order.begin(), order.end(), [](const View* a, const View* b) { return a->browser->isVisible() && !b->browser->isVisible(); });
    bool more = false;
    for (View* v : qAsConst(order)) {
        const qint64 left = kStepMs - clock.elapsed();
        if (left <= 0) {
            more = true;
            break;
        }
        more = stepView(*v, left, false) || more;
    }
    if (more)
        schedule(kStepEveryMs);
}

bool ChatLayout::isCopy(View& v, const QTextBlock& block) const
{
    const int tag = block.blockFormat().intProperty(layoutformat::kBlockTag);
    if (tag <= 0)
        return false;
    const QTextBlock previous = block.previous();
    if (previous.isValid() && previous.blockFormat().intProperty(layoutformat::kBlockTag) == tag)
        return true; // TeamSpeak appended it after a block of ours and copied that one's format
    const auto it = v.tagBlock.constFind(tag);
    if (it != v.tagBlock.constEnd() && it.value() != block.blockNumber() && it.value() < block.blockNumber()) {
        const QTextBlock other = v.document->findBlockByNumber(it.value());
        if (other.isValid() && other.blockFormat().intProperty(layoutformat::kBlockTag) == tag)
            return true;
    }
    return false;
}

bool ChatLayout::needsWork(View& v, int n) const
{
    const QTextBlock b = v.document->findBlockByNumber(n);
    if (!b.isValid())
        return false;
    const int tag = b.blockFormat().intProperty(layoutformat::kBlockTag);
    if (tag <= 0)
        return true; // not ours yet (or a format TeamSpeak copied from ours)
    if (v.tagEpoch.value(tag, -1) != v.epoch)
        return true; // styled for another look
    return isCopy(v, b);
}

bool ChatLayout::processBlock(View& v, int n)
{
    QTextDocument* doc = v.document;
    QTextBlock     b   = doc->findBlockByNumber(n);
    if (!b.isValid())
        return false;
    bool changed = false;
    int  tag     = b.blockFormat().intProperty(layoutformat::kBlockTag);
    if (tag > 0 && isCopy(v, b)) {
        changed = layoutdoc::sanitize(doc, n) || changed; // TeamSpeak's line with our block's format
        tag     = 0;
    } else if (layoutdoc::hasOurs(b)) {
        changed = layoutdoc::unstyle(doc, n) || changed; // ours, for another look (or an earlier instance's)
        if (layoutdoc::hasOurs(doc->findBlockByNumber(n)))
            changed = layoutdoc::sanitize(doc, n) || changed;
    }
    b                         = doc->findBlockByNumber(n);
    const layoutdoc::Row row  = layoutdoc::read(b);
    if (row.type == RowType::Unknown || row.styled)
        return changed;
    if (tag <= 0 || v.tagEpoch.contains(tag) == false) {
        if (tag <= 0)
            tag = v.nextTag++;
    }
    // The previous visible block: grouping, margins.
    layoutdoc::Neighbour previous;
    for (QTextBlock p = b.previous(); p.isValid(); p = p.previous()) {
        if (!p.isVisible())
            continue;
        previous.row     = layoutdoc::read(p);
        previous.valid   = true;
        previous.arrived = arrivedFor(v, p.blockNumber(), p.blockFormat().intProperty(layoutformat::kBlockTag));
        break;
    }
    const QDate           date    = dateFor(v, n);
    const qint64          arrived = arrivedFor(v, n, tag);
    const layoutdoc::Plan plan    = layoutdoc::plan(v.env, b, row, previous, date, arrived, tag);
    if (!layoutdoc::apply(doc, n, plan, v.env))
        return changed;
    v.tagEpoch.insert(tag, v.epoch);
    v.tagBlock.insert(tag, n);
    if (arrived > 0)
        v.arrivedByTag.insert(tag, arrived);
    if (!plan.name.isEmpty())
        v.rendered.remove(plan.name);
    if (row.reply)
        v.repliesTouched = true;
    return true;
}

QPair<int, int> ChatLayout::blocksShown(QTextBrowser* browser, qreal margin) const
{
    QTextDocument*               doc    = browser->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const qreal                  y      = browser->verticalScrollBar()->value();
    const qreal                  height = browser->viewport()->height();
    const int                    from   = layout->hitTest(QPointF(0, qMax(0.0, y - margin * height)), Qt::FuzzyHit);
    const int                    to     = layout->hitTest(QPointF(browser->viewport()->width(), y + height + margin * height), Qt::FuzzyHit);
    return {from < 0 ? 0 : doc->findBlock(from).blockNumber(), to < 0 ? doc->blockCount() - 1 : doc->findBlock(to).blockNumber()};
}

bool ChatLayout::stepView(View& v, qint64 budgetMs, bool all)
{
    QTextBrowser* browser = v.browser;
    if (!browser)
        return false;
    QTextDocument* doc = browser->document();
    if (v.document != doc) {
        documentSwapped(browser);
        return true;
    }
    settleBurst(v);
    if (m_mode == Mode::Classic) {
        if (v.historyNext >= 0 || !v.dirty.isEmpty() || !v.restyle.isEmpty()) {
            v.historyNext = -1;
            v.dirty.clear();
            v.restyle.clear();
            restoreView(v, false);
        }
        return false;
    }
    refreshEnv(v);
    const int count = doc->blockCount();
    const int floor = qMax(0, count - replydoc::kMaxBlocks);

    QElapsedTimer clock;
    clock.start();
    const auto timeLeft = [&] { return all || budgetMs < 0 || clock.elapsed() < budgetMs; };

    // What is on screen (a layout query: before the edits).
    const QPair<int, int> shown  = browser->isVisible() ? blocksShown(browser, 0.0) : QPair<int, int>(-1, -2);
    const chatscroll::Place anchor = chatscroll::capture(browser);

    bool         changed = false;
    bool         more    = false;
    QVector<int> styled; // blocks this step styled: their runs of events are planned again
    {
        layoutformat::MutatingScope mutating;
        QTextCursor                 batch(doc);
        batch.beginEditBlock();
        const auto run = [&](int n) {
            if (n < floor || n >= count)
                return;
            if (needsWork(v, n) && processBlock(v, n)) {
                changed = true;
                styled.append(n);
            }
        };
        // 1. what other modules asked for
        if (!v.restyle.isEmpty()) {
            QList<int> asked = v.restyle.values();
            std::sort(asked.begin(), asked.end(), std::greater<int>());
            v.restyle.clear();
            for (int i = 0; i < asked.size(); ++i) {
                if (!timeLeft()) {
                    for (int k = i; k < asked.size(); ++k)
                        v.restyle.insert(asked.at(k));
                    break;
                }
                const QTextBlock b = doc->findBlockByNumber(asked.at(i));
                if (b.isValid())
                    v.tagEpoch.remove(b.blockFormat().intProperty(layoutformat::kBlockTag)); // again, whatever it is now
                run(asked.at(i));
            }
        }
        // 2. the screen, bottom up
        for (int n = shown.second; n >= shown.first && n >= 0 && timeLeft(); --n)
            run(n);
        // 3. what TeamSpeak changed, newest first
        while (!v.dirty.isEmpty() && timeLeft()) {
            QPair<QTextCursor, QTextCursor>& range = v.dirty.last();
            const int                        first = range.first.block().blockNumber();
            int                              n     = range.second.block().blockNumber();
            for (; n >= first && timeLeft(); --n)
                run(n);
            if (n < first) {
                v.dirty.removeLast();
            } else {
                QTextBlock at = doc->findBlockByNumber(n);
                range.second.setPosition(at.position());
            }
        }
        // 4. the history, backwards
        while (v.historyNext >= floor && v.historyNext >= 0 && timeLeft())
            run(v.historyNext--);
        if (v.historyNext < floor)
            v.historyNext = -1;
        // 5. collapsed runs of events (P2) around what was styled
        if (!styled.isEmpty() && v.env.collapse)
            collapseRuns(v, styled, floor);
        batch.endEditBlock(); // TeamSpeak's view hears of it (contentsChange) while we are still mutating
    }
    more = !v.restyle.isEmpty() || !v.dirty.isEmpty() || v.historyNext >= 0;
    if (changed) {
        ++v.renderStamp;
        chatscroll::restore(browser, anchor);
        emit documentEdited(browser);
        if (v.repliesTouched && m_chat->m_replies) {
            v.repliesTouched = false;
            m_chat->m_replies->layoutChanged(browser); // reply lines: sizes for the new look, at once
        }
        if (v.hover >= 0)
            v.hoverSlot = slotOf(v, v.hover);
        browser->viewport()->update();
        if (m_barIn == browser && m_bar && m_bar->showing())
            placeBar(v);
        printIntro(browser);
    }
    return more;
}

void ChatLayout::restoreView(View& v, bool dropPictures)
{
    QTextBrowser* browser = v.browser;
    if (!browser)
        return;
    QTextDocument* doc = browser->document();
    const chatscroll::Place a = chatscroll::capture(browser);
    int            done = 0;
    {
        layoutformat::MutatingScope mutating;
        QStringList                 names;
        done = layoutdoc::restoreAll(doc, &names);
        for (auto it = v.rendered.constBegin(); it != v.rendered.constEnd(); ++it)
            names.append(it.key());
        for (const QString& name : qAsConst(names))
            doc->addResource(QTextDocument::ImageResource, QUrl(name), dropPictures ? QVariant() : QVariant(layoutdoc::blankPicture()));
    }
    v.rendered.clear();
    v.tagEpoch.clear();
    ++v.renderStamp;
    if (done > 0) {
        chatscroll::restore(browser, a);
        emit documentEdited(browser);
        if (m_chat->m_replies)
            m_chat->m_replies->layoutChanged(browser);
        browser->viewport()->update();
    }
}

void ChatLayout::unstyle(QTextBrowser* browser, int block)
{
    View* v = viewFor(browser);
    if (!v || v->document != browser->document())
        return;
    layoutformat::MutatingScope mutating;
    QTextDocument*              doc = browser->document();
    const QTextBlock            b   = doc->findBlockByNumber(block);
    if (!b.isValid() || !layoutdoc::hasOurs(b))
        return;
    v->tagEpoch.remove(b.blockFormat().intProperty(layoutformat::kBlockTag));
    layoutdoc::unstyle(doc, block);
    if (layoutdoc::hasOurs(doc->findBlockByNumber(block)))
        layoutdoc::sanitize(doc, block);
}

void ChatLayout::restyleSoon(QTextBrowser* browser, int block)
{
    View* v = viewFor(browser);
    if (!v)
        return;
    v->restyle.insert(block);
    schedule(0);
}

void ChatLayout::visibilityChanged(QTextBrowser* browser, const QSet<int>& blocks)
{
    View* v = viewFor(browser);
    if (!v || !v->document)
        return;
    for (const int n : blocks) {
        v->restyle.insert(n);
        for (QTextBlock b = v->document->findBlockByNumber(n).next(); b.isValid(); b = b.next()) {
            if (b.isVisible()) {
                v->restyle.insert(b.blockNumber()); // it may join the message before it now (or stop to)
                break;
            }
        }
    }
    schedule(0);
}

void ChatLayout::pendingChanged()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser)
            it->browser->viewport()->update();
    }
}

// ============================================================================================
// Pictures and painting
// ============================================================================================

void ChatLayout::renderVisible(View& v)
{
    QTextBrowser* browser = v.browser;
    if (!browser || m_mode == Mode::Classic || v.document != browser->document())
        return;
    QTextDocument*        doc   = v.document;
    const QPair<int, int> shown = blocksShown(browser, 0.25);
    // Hover paints come often: nothing to do while neither the lines on screen nor what they show changed.
    if (v.renderedStamp == v.renderStamp && v.renderedShown == shown)
        return;
    QElapsedTimer clock;
    clock.start();
    bool drew       = false;
    bool left       = false;
    bool incomplete = false;
    for (int n = shown.first; n <= shown.second && n >= 0; ++n) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isVisible())
            continue;
        // Run chips (P2): "x3", "+5 more events", "Show fewer".
        if (b.blockFormat().boolProperty(layoutformat::kRunHead)) {
            for (const layoutdoc::ChipRef& chip : layoutdoc::chipsOf(b)) {
                const bool    hot = chip.toggle && v.chipHover == n;
                const QString sig = QString::fromLatin1("chip|%1|%2|%3|%4").arg(chip.text).arg(hot ? 1 : 0).arg(v.env.dpr).arg(v.env.colors.base.name());
                const auto    now = v.rendered.constFind(chip.name);
                if (now != v.rendered.constEnd() && now->signature == sig)
                    continue;
                doc->addResource(QTextDocument::ImageResource, QUrl(chip.name), QPixmap::fromImage(layoutdoc::chipPicture(chip.text, hot, v.env, nullptr)));
                v.rendered.insert(chip.name, {sig, b.blockFormat().intProperty(layoutformat::kBlockTag)});
                drew = true;
            }
        }
        const layoutformat::Lead lead = layoutformat::leadOf(b);
        if (!lead.styled || lead.kind == layoutformat::Continuation)
            continue;
        const QString name = lead.format.toImageFormat().name();
        layoutdoc::Look look;
        if (lead.kind == layoutformat::Head && n == v.replyPressBlock)
            look.replyPressed = v.replyPressed;
        if (lead.kind == layoutformat::Head && n == v.hover)
            look.replyHover = v.replyHover;
        if (lead.kind == layoutformat::Head && m_mode == Mode::Cozy && Settings::instance().chatAvatars) {
            const layoutdoc::Row row = layoutdoc::read(b); // avatars (P2): TeamSpeak's, else initials
            look.avatar              = avatarFor(v, row.uid);
            replyart::Header header;
            if (row.reply && v.env.replyHeader && v.env.replyHeader(n, &header))
                look.replyAvatar = avatarFor(v, header.uid);
        }
        const QDate   date = dateFor(v, n);
        const QString sig  = layoutdoc::signature(v.env, b, date, look);
        if (sig.isEmpty())
            continue;
        const auto now = v.rendered.constFind(name);
        if (now != v.rendered.constEnd() && now->signature == sig)
            continue;
        if (clock.elapsed() > kPaintBudgetMs) {
            left = true; // drawn with the next paint
            break;
        }
        emoji::takeTextFallbacks();
        const QImage image = layoutdoc::render(v.env, b, date, look, nullptr);
        if (image.isNull())
            continue;
        doc->addResource(QTextDocument::ImageResource, QUrl(name), QPixmap::fromImage(image));
        // Emoji in a name still on their way (emojitext.h): drawn again once they are there.
        const bool complete = emoji::takeTextFallbacks() == 0;
        v.rendered.insert(name, {complete ? sig : QString(), b.blockFormat().intProperty(layoutformat::kBlockTag)});
        incomplete = incomplete || !complete;
        drew       = true;
    }
    if (drew && v.rendered.size() > kMaxRendered)
        evict(v);
    if (!left && !incomplete) {
        v.renderedStamp = v.renderStamp;
        v.renderedShown = shown;
    }
    if (left) {
        // Posted to us (never to TeamSpeak's viewport): dropped with us at unload.
        QMetaObject::invokeMethod(
            this,
            [vp = QPointer<QWidget>(browser->viewport())] {
                if (vp)
                    vp->update();
            },
            Qt::QueuedConnection);
    }
}

void ChatLayout::evict(View& v)
{
    QTextBrowser* browser = v.browser;
    if (!browser)
        return;
    QTextDocument*        doc  = v.document;
    const QPair<int, int> keep = blocksShown(browser, 0.5);
    const QPixmap         blank = layoutdoc::blankPicture();
    for (auto it = v.rendered.begin(); it != v.rendered.end();) {
        const int  n    = v.tagBlock.value(it->tag, -1);
        const bool near = n >= keep.first && n <= keep.second;
        const bool same = n >= 0 && doc->findBlockByNumber(n).blockFormat().intProperty(layoutformat::kBlockTag) == it->tag;
        if (near || !same) {
            ++it;
            continue;
        }
        doc->addResource(QTextDocument::ImageResource, QUrl(it.key()), blank);
        it = v.rendered.erase(it);
    }
}

QRect ChatLayout::slotOf(View& v, int block) const
{
    QTextBrowser* browser = v.browser;
    QTextDocument* doc    = browser->document();
    const QTextBlock b    = doc->findBlockByNumber(block);
    if (!b.isValid() || !b.isVisible())
        return {};
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const QRectF                 r      = layout->blockBoundingRect(b);
    qreal                        bottom = r.bottom();
    for (QTextBlock n = b.next(); n.isValid(); n = n.next()) {
        if (!n.isVisible())
            continue;
        const qreal gap = qMax(n.blockFormat().topMargin(), b.blockFormat().bottomMargin());
        bottom          = qMax(bottom, layout->blockBoundingRect(n).top() - gap);
        break;
    }
    const int sy = browser->verticalScrollBar()->value();
    const int top = static_cast<int>(std::floor(r.top())) - sy;
    return QRect(0, top, browser->viewport()->width(), qMax(1, static_cast<int>(std::ceil(bottom)) - sy - top));
}

void ChatLayout::paintLayer(View& v, QPainter& p, const QRegion& region, bool over)
{
    QTextBrowser* browser = v.browser;
    if (!browser || m_mode == Mode::Classic)
        return;
    QTextDocument*           doc  = browser->document();
    const layoutart::Colors& c    = v.env.colors;
    const layoutart::Tokens& t    = v.env.tokens;
    const qreal              sx   = browser->horizontalScrollBar()->value();
    const qreal              sy   = browser->verticalScrollBar()->value();
    const qreal              left = doc->documentMargin() - sx;
    p.save();
    p.setClipRegion(region);
    const QPair<int, int> shown = blocksShown(browser, 0.0); // once per paint (a layout query walks the blocks)
    if (over) {
        // Over the text: not over pictures (previews, albums, cards) and reply rows.
        QRegion clip = region;
        for (int n = shown.first; n <= shown.second && n >= 0; ++n) {
            for (const QRectF& r : m_chat->mediaRectsIn(browser, n))
                clip -= r.toAlignedRect();
        }
        p.setClipRegion(clip);
    }
    // Rows that mention you (P2): a tint and a 3 px bar on the left, under everything else.
    for (int n = shown.first; n <= shown.second && n >= 0; ++n) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isVisible() || !layoutdoc::isMention(b))
            continue;
        const QRect r = slotOf(v, n);
        if (r.isEmpty() || !region.intersects(r))
            continue;
        p.fillRect(r, c.mentionTint);
        p.fillRect(QRect(r.left(), r.top(), 3, r.height()), c.mentionBar);
    }
    const int focus = m_chat->m_replies ? m_chat->m_replies->pendingBlock(browser) : -1;
    if (focus >= 0) {
        const QRect r = slotOf(v, focus);
        if (!r.isEmpty()) {
            p.fillRect(r, c.hover);
            p.fillRect(QRect(r.left(), r.top(), 2, r.height()), c.focusBar);
        }
    }
    if (v.hover >= 0 && v.hover != focus && !v.probing)
        p.fillRect(v.hoverSlot, c.hover);

    const auto lineBaseline = [&](const QTextBlock& b, int position, qreal* baseline) {
        const QTextLayout* tl = b.layout();
        if (!tl || tl->lineCount() == 0)
            return false;
        const QTextLine line = tl->lineForTextPosition(position - b.position());
        if (!line.isValid())
            return false;
        const QRectF br = doc->documentLayout()->blockBoundingRect(b);
        *baseline       = br.top() + line.y() + line.ascent() - sy;
        return true;
    };
    // The time of a hovered continuation, in the gutter.
    if (v.hover >= 0) {
        const QTextBlock         b    = doc->findBlockByNumber(v.hover);
        const layoutformat::Lead lead = layoutformat::leadOf(b);
        if (lead.styled && lead.kind == layoutformat::Continuation) {
            const QString time = layoutart::shortTime(lead.format.stringProperty(layoutformat::kTime), t.ampm);
            qreal         base = 0;
            if (!time.isEmpty() && lineBaseline(b, lead.bodyStart, &base)) {
                if (m_mode == Mode::Compact)
                    layoutart::paintGutterTime(p, time, left, base, t, c, true);
                else
                    layoutart::paintGutterTime(p, time, left + t.G - 8, base, t, c, false);
            }
        }
    }
    // Compact replies: the time in the gutter on the name's line, always.
    if (m_mode == Mode::Compact) {
        for (int n = shown.first; n <= shown.second && n >= 0; ++n) {
            const QTextBlock         b    = doc->findBlockByNumber(n);
            const layoutformat::Lead lead = layoutformat::leadOf(b);
            if (!lead.styled || lead.kind != layoutformat::CompactHead || lead.reply < 0 || !b.isVisible())
                continue;
            qreal         base = 0;
            const QString time = layoutart::shortTime(lead.format.stringProperty(layoutformat::kTime), t.ampm);
            if (!time.isEmpty() && lineBaseline(b, lead.object, &base))
                layoutart::paintGutterTime(p, time, left, base, t, c, true);
        }
    }
    // The hovered link: a full underline under the text's own (40 %) one.
    for (const QRectF& r : qAsConst(v.linkRects))
        p.fillRect(QRectF(r.left() - sx, r.top() - sy, r.width(), r.height()), c.link);
    p.restore();
}

void ChatLayout::checkPaintUnder(View& v)
{
    if (v.checked || !v.browser || v.hover < 0)
        return;
    v.checked              = true;
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    if (doc->rootFrame()->frameFormat().background().style() != Qt::NoBrush) {
        v.overlay = true; // the document paints its own background over ours
        ensureOverlay(v);
        return;
    }
    // A pixel of the hovered row where there is no text, with and without our tint (the viewport alone).
    QWidget*    vp = browser->viewport();
    const QRect probe(vp->width() - 3, v.hoverSlot.center().y(), 1, 1);
    if (!vp->rect().contains(probe))
        return;
    QImage with(1, 1, QImage::Format_ARGB32_Premultiplied);
    QImage without(1, 1, QImage::Format_ARGB32_Premultiplied);
    with.fill(Qt::transparent);
    without.fill(Qt::transparent);
    vp->render(&with, QPoint(), QRegion(probe), QWidget::DrawWindowBackground);
    v.probing = true;
    vp->render(&without, QPoint(), QRegion(probe), QWidget::DrawWindowBackground);
    v.probing = false;
    if (with.pixel(0, 0) == without.pixel(0, 0)) {
        v.overlay = true; // the skin painted over it
        ensureOverlay(v);
    }
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QString::fromLatin1("[test] layout: paint-under check %1 (%2 vs %3)")
                 .arg(v.overlay ? QString::fromLatin1("failed, overlay") : QString::fromLatin1("ok"), QColor(with.pixel(0, 0)).name(), QColor(without.pixel(0, 0)).name()));
#endif
}

void ChatLayout::ensureOverlay(View& v)
{
    if (!v.browser || v.overlayWidget)
        return;
    const QPointer<QTextBrowser> guard(v.browser);
    auto*                        overlay = new Overlay(v.browser->viewport(), [this, guard](QPainter& p, const QRegion& region) {
        if (View* w = guard ? viewFor(guard.data()) : nullptr)
            paintLayer(*w, p, region, true);
    });
    v.overlayWidget = overlay;
    if (m_bar)
        m_bar->raise();
}

// ============================================================================================
// The pointer
// ============================================================================================

int ChatLayout::blockAt(View& v, const QPoint& pos, QRect* slot) const
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    const QPointF  docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return -1;
    const QTextBlock b = doc->findBlock(hit);
    for (const QTextBlock& candidate : {b, b.previous(), b.next()}) {
        if (!candidate.isValid() || !candidate.isVisible())
            continue;
        const QRect r = slotOf(const_cast<View&>(v), candidate.blockNumber());
        if (r.contains(QPoint(qBound(0, pos.x(), qMax(0, r.width() - 1)), pos.y()))) {
            if (slot)
                *slot = r;
            return candidate.blockNumber();
        }
    }
    return -1;
}

bool ChatLayout::replyRowAt(View& v, const QPoint& pos, int* block) const
{
    if (m_mode != Mode::Cozy)
        return false;
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    const QPointF  docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return false;
    const QTextBlock         b    = doc->findBlock(hit);
    const layoutformat::Lead lead = layoutformat::leadOf(b);
    if (!lead.styled || lead.kind != layoutformat::Head || lead.reply < 0)
        return false;
    layoutart::HeadGeometry g;
    QSizeF                  size;
    if (!layoutdoc::headGeometry(v.env, b, const_cast<ChatLayout*>(this)->dateFor(const_cast<View&>(v), b.blockNumber()), &g, &size) || g.replyRow.isEmpty())
        return false;
    const QRectF br  = doc->documentLayout()->blockBoundingRect(b);
    if (!b.layout() || b.layout()->lineCount() == 0)
        return false; // not laid out (QTextLine's methods need a line)
    const QTextLine line = b.layout()->lineAt(0);
    const QRectF picture(br.left() + line.cursorToX(lead.object - b.position()), br.top() + pictureTop(line, lead.format.toImageFormat()), size.width(), size.height());
    const QRectF row = g.replyRow.translated(picture.topLeft());
    if (!row.contains(docPos))
        return false;
    if (block)
        *block = b.blockNumber();
    return true;
}

void ChatLayout::updateHover(View& v, const QPoint& pos, bool moved)
{
    Q_UNUSED(moved);
    QTextBrowser* browser = v.browser;
    if (!browser || m_mode == Mode::Classic)
        return;
    QWidget* vp = browser->viewport();
    QRect    slot;
    const int n = blockAt(v, pos, &slot);

    // The hovered link (a body link: not a name, not a file shown as a preview).
    QVector<QRectF> links;
    const QString   href = browser->anchorAt(pos);
    if (!href.isEmpty() && n >= 0) {
        QTextDocument*   doc = browser->document();
        const QTextBlock b   = doc->findBlockByNumber(n);
        const QRectF     br  = doc->documentLayout()->blockBoundingRect(b);
        const QPointF    docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
        const int        hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
        for (auto it = b.begin(); !it.atEnd() && b.layout(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !isBodyLink(f.charFormat()) || f.charFormat().anchorHref() != href)
                continue;
            if (hit < f.position() - 64 || hit > f.position() + f.length() + 64)
                continue;
            const QFontMetricsF fm(f.charFormat().font().resolve(doc->defaultFont()));
            const int           from = f.position() - b.position();
            const int           to   = from + f.length();
            for (int i = 0; i < b.layout()->lineCount(); ++i) {
                const QTextLine line = b.layout()->lineAt(i);
                const int       a    = qMax(from, line.textStart());
                const int       z    = qMin(to, line.textStart() + line.textLength());
                if (a >= z)
                    continue;
                const qreal x1 = line.cursorToX(a);
                const qreal x2 = line.cursorToX(z);
                const qreal y  = br.top() + line.y() + line.ascent() + qMax(1.0, fm.underlinePos());
                links.append(QRectF(br.left() + qMin(x1, x2), std::floor(y), qAbs(x2 - x1), qMax(1.0, std::round(fm.lineWidth()))));
            }
        }
    }
    bool sameLinks = links.size() == v.linkRects.size();
    for (int i = 0; sameLinks && i < links.size(); ++i)
        sameLinks = links.at(i) == v.linkRects.at(i);
    if (!sameLinks) {
        const qreal sx = browser->horizontalScrollBar()->value();
        const qreal sy = browser->verticalScrollBar()->value();
        for (const QRectF& r : qAsConst(v.linkRects))
            vp->update(r.translated(-sx, -sy).toAlignedRect().adjusted(-1, -1, 1, 1));
        for (const QRectF& r : qAsConst(links))
            vp->update(r.translated(-sx, -sy).toAlignedRect().adjusted(-1, -1, 1, 1));
        v.linkRects = links;
        v.linkHref  = href;
        if (v.overlayWidget)
            v.overlayWidget->update();
    }

    // A Cozy head's reply row: the hand, the name underlined, a click jumps.
    int        replyBlock = -1;
    const bool reply      = replyRowAt(v, pos, &replyBlock) && replyBlock == n;
    if (reply != v.replyHover) {
        v.replyHover = reply;
        ++v.renderStamp;
        vp->update(slot.isEmpty() ? v.hoverSlot : slot);
    }
    // A run's chip (P2): the hand, its hover look; a click opens or closes the run.
    const int chip = chipAt(v, pos);
    if (chip != v.chipHover) {
        if (v.chipHover >= 0)
            vp->update(slotOf(v, v.chipHover));
        v.chipHover = chip;
        ++v.renderStamp;
        if (chip >= 0)
            vp->update(slotOf(v, chip));
    }
    const bool hand = reply || chip >= 0;
    if (hand != v.handCursor) {
        v.handCursor = hand;
        m_chat->updateCursor(browser, hand, pos, true);
    }

    if (n != v.hover || slot != v.hoverSlot) {
        if (!v.hoverSlot.isEmpty())
            vp->update(v.hoverSlot);
        if (!slot.isEmpty())
            vp->update(slot);
        if (v.overlayWidget) {
            v.overlayWidget->update(v.hoverSlot);
            v.overlayWidget->update(slot);
        }
        v.hover     = n;
        v.hoverSlot = slot;
        if (n >= 0 && !v.checked)
            QTimer::singleShot(0, this, [this, guard = QPointer<QTextBrowser>(browser)] {
                if (View* w = guard ? viewFor(guard.data()) : nullptr)
                    checkPaintUnder(*w);
            });
    }
    placeBar(v);
}

void ChatLayout::clearHover(View& v)
{
    if (!v.browser)
        return;
    QWidget* vp = v.browser->viewport();
    if (!v.hoverSlot.isEmpty())
        vp->update(v.hoverSlot);
    if (v.overlayWidget)
        v.overlayWidget->update();
    if (!v.linkRects.isEmpty())
        vp->update();
    if (v.handCursor)
        m_chat->updateCursor(v.browser, false, QPoint(-1, -1));
    if (v.chipHover >= 0)
        vp->update(slotOf(v, v.chipHover));
    v.hover      = -1;
    v.hoverSlot  = QRect();
    v.linkRects.clear();
    v.replyHover = false;
    v.chipHover  = -1;
    v.handCursor = false;
    ++v.renderStamp;
    if (m_barIn == v.browser)
        hideBar();
}

QPoint ChatLayout::bodyPoint(View& v, int block) const
{
    QTextBrowser*    browser = v.browser;
    QTextDocument*   doc     = browser->document();
    const QTextBlock b       = doc->findBlockByNumber(block);
    if (!b.isValid() || !b.layout())
        return {};
    const int       at   = layoutformat::bodyStart(b) - b.position();
    const QTextLine line = b.layout()->lineForTextPosition(qMax(0, at));
    if (!line.isValid())
        return {};
    const QRectF br = doc->documentLayout()->blockBoundingRect(b);
    const qreal  x  = br.left() + line.cursorToX(at) + 2;
    const qreal  y  = br.top() + line.y() + line.height() / 2.0;
    return QPoint(qRound(x) - browser->horizontalScrollBar()->value(), qRound(y) - browser->verticalScrollBar()->value());
}

void ChatLayout::placeBar(View& v)
{
    QTextBrowser* browser = v.browser;
    const Settings& s     = Settings::instance();
    const bool held       = QApplication::mouseButtons() & Qt::LeftButton;
    bool       ok         = browser && m_mode != Mode::Classic && s.chatHoverActions && v.hover >= 0 && !held && !QApplication::activePopupWidget()
                 && browser->viewport()->width() >= kMinBarWidth && browser->viewport()->rect().intersects(v.hoverSlot);
    if (ok) {
        const layoutdoc::Row row = layoutdoc::read(browser->document()->findBlockByNumber(v.hover));
        ok                       = row.type == RowType::Message; // dividers get nothing, system rows the tint only
    }
    if (!ok) {
        if (m_barIn == browser)
            hideBar();
        return;
    }
    QWidget* vp = browser->viewport();
    if (!m_bar) {
        m_bar = new ActionBar(vp);
        connect(m_bar, &ActionBar::triggered, this, &ChatLayout::barAction);
        m_bar->installEventFilter(this);
    } else if (m_bar->parentWidget() != vp) {
        m_bar->hide();
        m_bar->setParent(vp);
    }
    if (m_barIn != browser || m_barBlock != v.hover)
        m_bar->setLook(v.env.colors, m_mode == Mode::Compact);
    const QString key = m_chat->reactableKeyIn(browser, v.hover);
    m_bar->setReactVisible(!key.isEmpty() && Settings::instance().showReactions);
    m_barIn    = browser;
    m_barBlock = v.hover;
    const QRect box  = m_bar->barRect();
    int         x    = vp->width() - kBarRight - box.width();
    int         y    = v.hoverSlot.top() - box.height() / 2;
    QRect       want(x, y, box.width(), box.height());
    for (const QRectF& media : m_chat->mediaRectsIn(browser, v.hover)) {
        if (media.toAlignedRect().intersects(want)) {
            want.moveTop(v.hoverSlot.top() - box.height() - 2); // fully above the row, never over its media
            break;
        }
    }
    want.moveTop(qBound(2, want.top(), qMax(2, vp->height() - box.height() - 2)));
    want.moveLeft(qBound(2, want.left(), qMax(2, vp->width() - box.width() - 2)));
    m_bar->move(want.topLeft() - box.topLeft());
    m_bar->showBar(true);
    m_bar->raise();
}

void ChatLayout::hideBar(bool animate)
{
    if (m_bar)
        m_bar->hideBar(animate);
    m_barBlock = -1;
}

void ChatLayout::barAction(int action)
{
    QTextBrowser* browser = m_barIn;
    View*         v       = viewFor(browser);
    if (!v || m_barBlock < 0)
        return;
    const int block = m_barBlock;
    switch (action) {
    case ActionBar::React: {
        const QString key = m_chat->reactableKeyIn(browser, block);
        QAbstractButton* button = m_bar ? m_bar->button(ActionBar::React) : nullptr;
        if (!key.isEmpty() && m_chat->m_reactions && button)
            m_chat->m_reactions->openPickerAt(browser, key, QRect(button->mapToGlobal(QPoint(0, 0)), button->size()));
        break;
    }
    case ActionBar::Reply:
        if (m_chat->m_replies)
            m_chat->m_replies->startReplyAt(browser, block);
        break;
    case ActionBar::Copy: {
        const QTextBlock b = browser->document()->findBlockByNumber(block);
        if (b.isValid() && m_chat->m_emoji) {
            const replydoc::Message m    = replydoc::parseBlock(b); // after the reply line's quote too
            const int               from = m.block >= 0 ? m.textStart : layoutformat::bodyStart(b);
            m_chat->m_emoji->copyText(browser, from, b.position() + b.length() - 1);
            if (m_bar)
                m_bar->flashCopied();
        }
        break;
    }
    case ActionBar::More: {
        // TeamSpeak's own menu for the message, with TS Media's items in it.
        const QPoint at = bodyPoint(*v, block);
        QWidget*     vp = browser->viewport();
        QCoreApplication::postEvent(vp, new QContextMenuEvent(QContextMenuEvent::Mouse, at, vp->mapToGlobal(at)));
        hideBar(false);
        break;
    }
    default:
        break;
    }
}

bool ChatLayout::hoverBlock(QTextBrowser* browser, int block, QPoint* at)
{
    View* v = viewFor(browser);
    if (!v)
        return false;
    const QPoint pos = bodyPoint(*v, block);
    if (pos.isNull())
        return false;
    updateHover(*v, pos, true);
    if (at)
        *at = pos;
    return v->hover == block;
}

void ChatLayout::keyboardMenu(QTextBrowser* browser)
{
    // The menu key or Shift+F10: TeamSpeak's menu for the message being replied to, the hovered one or the
    // one with the text cursor gets TS Media's items too.
    View* v = viewFor(browser);
    if (!v || !m_chat->m_replies)
        return;
    int block = m_chat->m_replies->pendingBlock(browser);
    if (block < 0)
        block = v->hover;
    if (block < 0)
        block = browser->textCursor().block().blockNumber();
    const QPoint at = bodyPoint(*v, block);
    if (at.isNull())
        return;
    m_chat->m_replies->prepareMenu(browser, at, browser->viewport()->mapToGlobal(at));
}

bool ChatLayout::toolTip(View& v, QEvent* event)
{
    auto*          he      = static_cast<QHelpEvent*>(event);
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    const QPointF  docPos(he->pos().x() + browser->horizontalScrollBar()->value(), he->pos().y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return false;
    const QTextBlock         b    = doc->findBlock(hit);
    const layoutformat::Lead lead = layoutformat::leadOf(b);
    if (!lead.styled || (lead.kind != layoutformat::Head && lead.kind != layoutformat::CompactHead && lead.kind != layoutformat::SystemPrefix))
        return false;
    const QTextImageFormat image = lead.format.toImageFormat();
    const QRectF           br    = doc->documentLayout()->blockBoundingRect(b);
    const QTextLine        line  = b.layout()->lineForTextPosition(lead.object - b.position());
    if (!line.isValid())
        return false;
    const QRectF picture(br.left() + line.cursorToX(lead.object - b.position()), br.top() + pictureTop(line, image), image.width(),
                         image.height());
    if (!picture.contains(docPos))
        return false;
    const QPointF local = docPos - picture.topLeft();
    const QDate   date  = dateFor(v, b.blockNumber());
    const QString time  = lead.format.stringProperty(layoutformat::kTime);
    const QPoint  shift(-browser->horizontalScrollBar()->value(), -browser->verticalScrollBar()->value());
    QString       text;
    QRectF        area;
    if (lead.kind == layoutformat::Head) {
        layoutart::HeadGeometry g;
        QSizeF                  size;
        layoutdoc::headGeometry(v.env, b, date, &g, &size);
        if (!g.replyRow.isEmpty() && g.replyRow.contains(local) && m_chat->m_replies) {
            text = m_chat->m_replies->replyToolTip(browser, b.blockNumber());
            area = g.replyRow;
        } else if (!g.time.isEmpty() && g.time.adjusted(-2, 0, 2, 0).contains(local)) {
            text = layoutart::fullTimeLabel(date, time).toHtmlEscaped();
            area = g.time;
        } else if (g.elided && g.name.contains(local)) {
            text = layoutdoc::read(b).nick.toHtmlEscaped(); // the whole nickname (untrusted: escaped)
            area = g.name;
        }
    } else if (local.x() < v.env.tokens.L || lead.kind == layoutformat::SystemPrefix) {
        // A compact head's or a system row's time.
        if (!time.isEmpty()) {
            text = layoutart::fullTimeLabel(date, time).toHtmlEscaped();
            area = QRectF(0, 0, lead.kind == layoutformat::SystemPrefix ? image.width() : v.env.tokens.L, image.height());
        }
    }
    if (text.isEmpty())
        return false;
    // Rich text built from i18n / escaped strings only (QToolTip's label outlives the plugin).
    QToolTip::showText(he->globalPos(), QString::fromLatin1("<div style='white-space:pre'>%1</div>").arg(text), browser->viewport(),
                       area.translated(picture.topLeft()).translated(shift).toAlignedRect());
    return true;
}

void ChatLayout::printIntro(QTextBrowser* browser)
{
    if (m_introDone || m_mode == Mode::Classic || !browser || !browser->isVisible())
        return;
    const uint64 sch = ts3::currentConnection();
    if (!sch || !ts3::isConnected(sch))
        return;
    m_introDone   = true;
    Settings& s   = Settings::instance();
    s.chatLayoutIntroShown = true;
    s.save();
    ts3::print(sch, i18n::t("[b]TS Media chat:[/b] new chat layout. Settings > Chat layout switches back to TeamSpeak classic."));
}

// ============================================================================================
// Avatars (P2)
// ============================================================================================

namespace {

constexpr int    kAvatarPixels   = 48;            // decoded at this size (24 px at 2x)
constexpr int    kMaxAvatars     = 64;
constexpr qint64 kMaxAvatarBytes = 1024 * 1024;
constexpr qint64 kRecheckMs      = 5 * 60 * 1000; // a missing avatar is looked for again after this

// TeamSpeak's cache file of a client's avatar: <config>/cache/<base64 server uid>/clients/avatar_<letters>,
// the letters being the uid's bytes (20) in hex with 0-f written as a-p. Only a-p and base64 characters: no
// way out of the cache folder.
QString avatarPath(const QString& serverUid, const QString& uid)
{
    const QString config = ts3::configDir();
    const QByteArray raw = QByteArray::fromBase64(uid.toLatin1());
    if (config.isEmpty() || raw.isEmpty() || raw.size() > 64 || serverUid.isEmpty()) // (20 bytes for a real uid)
        return {};
    QByteArray letters = raw.toHex();
    for (char& c : letters)
        c = static_cast<char>('a' + (c <= '9' ? c - '0' : c - 'a' + 10));
    return config + QLatin1String("/cache/") + QString::fromLatin1(serverUid.toUtf8().toBase64()) + QLatin1String("/clients/avatar_") + QString::fromLatin1(letters);
}

// On the worker: the first frame, cropped to a centred square, kAvatarPixels big (null if it can't be read).
QImage decodeAvatar(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > kMaxAvatarBytes)
        return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true); // TeamSpeak's files have no extension
    const QSize source = reader.size();
    if (source.isValid() && (source.width() > 4096 || source.height() > 4096))
        return {};
    if (source.isValid() && source.width() > 0 && source.height() > 0) {
        const qreal scale = qMax(kAvatarPixels / qreal(source.width()), kAvatarPixels / qreal(source.height()));
        if (scale < 1.0)
            reader.setScaledSize(QSize(qMax(1, qRound(source.width() * scale)), qMax(1, qRound(source.height() * scale))));
    }
    QImage image = reader.read();
    if (image.isNull())
        return {};
    image = image.scaled(kAvatarPixels, kAvatarPixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect square((image.width() - kAvatarPixels) / 2, (image.height() - kAvatarPixels) / 2, kAvatarPixels, kAvatarPixels);
    return image.copy(square).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace

QImage ChatLayout::avatarFor(View& v, const QString& uid)
{
    if (uid.isEmpty() || v.serverUid.isEmpty() || !Settings::instance().chatAvatars)
        return {};
    const QString key = v.serverUid + QLatin1Char('|') + uid;
    const qint64  now = QDateTime::currentMSecsSinceEpoch();
    auto          it  = m_avatars.find(key);
    if (it != m_avatars.end()) {
        m_avatarOrder.removeOne(key);
        m_avatarOrder.append(key);
        if (!it->image.isNull() || it->loading || now - it->checkedMs < kRecheckMs)
            return it->image;
    }
    const QString path = avatarPath(v.serverUid, uid);
    Avatar&       a    = m_avatars[key];
    a.checkedMs        = now;
    if (path.isEmpty())
        return a.image;
    a.loading = true;
    if (!m_avatarOrder.contains(key))
        m_avatarOrder.append(key);
    m_pool->start(QRunnable::create([this, key, path] {
        const QImage image = decodeAvatar(path); // untrusted file: bounded size, QImageReader only
        QMetaObject::invokeMethod(this, [this, key, image] { avatarLoaded(key, image); }, Qt::QueuedConnection);
    }));
    return a.image;
}

void ChatLayout::avatarLoaded(const QString& key, const QImage& image)
{
    auto it = m_avatars.find(key);
    if (it == m_avatars.end())
        return;
    it->loading   = false;
    it->image     = image;
    it->checkedMs = QDateTime::currentMSecsSinceEpoch();
    while (m_avatarOrder.size() > kMaxAvatars)
        m_avatars.remove(m_avatarOrder.takeFirst());
    if (image.isNull())
        return;
    for (auto v = m_views.begin(); v != m_views.end(); ++v) {
        ++v->renderStamp;
        if (v->browser && v->browser->isVisible())
            v->browser->viewport()->update(); // its heads are drawn again (the picture's signature changed)
    }
}

void ChatLayout::avatarUpdated(quint64 sch, int clientId)
{
    const QString uid    = ts3::clientUid(sch, static_cast<anyID>(clientId));
    const QString server = ts3::serverUid(sch);
    if (uid.isEmpty() || server.isEmpty())
        return;
    const QString key = server + QLatin1Char('|') + uid;
    m_avatars.remove(key);
    m_avatarOrder.removeOne(key);
    for (auto v = m_views.begin(); v != m_views.end(); ++v) {
        ++v->renderStamp;
        if (v->browser && v->browser->isVisible())
            v->browser->viewport()->update();
    }
}

// ============================================================================================
// Collapsed runs of events (P2)
// ============================================================================================

void ChatLayout::collapseRuns(View& v, QVector<int> blocks, int floor)
{
    QTextDocument* doc = v.document;
    if (!doc || blocks.isEmpty())
        return;
    std::sort(blocks.begin(), blocks.end());
    int covered = -1;
    int serial  = v.chipSerial;
    for (const int n : qAsConst(blocks)) {
        if (n <= covered)
            continue;
        covered = n;
        for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(doc, n, n, v.expandedRuns, floor)) {
            layoutdoc::applyRun(doc, run, v.env, &serial);
            if (!run.rows.isEmpty())
                covered = qMax(covered, run.rows.last());
        }
    }
    v.chipSerial = serial;
}

int ChatLayout::chipAt(View& v, const QPoint& pos) const
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    const QPointF  docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return -1;
    const QTextBlock b = doc->findBlock(hit);
    if (!b.isVisible() || !b.blockFormat().boolProperty(layoutformat::kRunHead) || !b.layout())
        return -1;
    const QRectF br = doc->documentLayout()->blockBoundingRect(b);
    for (const layoutdoc::ChipRef& chip : layoutdoc::chipsOf(b)) {
        if (!chip.toggle)
            continue;
        const QTextLine line = b.layout()->lineForTextPosition(chip.position - b.position());
        if (!line.isValid())
            continue;
        QTextCursor c(doc);
        c.setPosition(chip.position);
        c.setPosition(chip.position + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat image = c.charFormat().toImageFormat();
        const QRectF           rect(br.left() + line.cursorToX(chip.position - b.position()) + 6, br.top() + pictureTop(line, image), image.width() - 6, image.height());
        if (rect.contains(docPos))
            return b.blockNumber();
    }
    return -1;
}

void ChatLayout::toggleRun(View& v, int block)
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = v.document;
    if (!browser || !doc || doc != browser->document())
        return;
    const int                     floor = qMax(0, doc->blockCount() - replydoc::kMaxBlocks);
    const QVector<layoutdoc::RunPlan> before = layoutdoc::planRuns(doc, block, block, v.expandedRuns, floor);
    if (before.isEmpty() || before.first().tag <= 0)
        return;
    const int tag = before.first().tag;
    if (v.expandedRuns.contains(tag))
        v.expandedRuns.remove(tag);
    else
        v.expandedRuns.insert(tag);
    v.env.expandedRuns = v.expandedRuns;
    // The clicked row stays where it is (the rows below it come or go); a chat at the bottom stays there.
    const chatscroll::RowPlace place = chatscroll::captureRow(browser, block);
    {
        layoutformat::MutatingScope mutating;
        QTextCursor                 batch(doc);
        batch.beginEditBlock();
        int serial = v.chipSerial;
        for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(doc, block, block, v.expandedRuns, floor))
            layoutdoc::applyRun(doc, run, v.env, &serial);
        v.chipSerial = serial;
        batch.endEditBlock();
    }
    chatscroll::restoreRow(browser, place);
    emit documentEdited(browser);
    ++v.renderStamp;
    if (v.hover >= 0)
        v.hoverSlot = slotOf(v, v.hover);
    browser->viewport()->update();
}

// ============================================================================================
// Events
// ============================================================================================

bool ChatLayout::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    switch (type) {
    case QEvent::Paint:
    case QEvent::MouseMove:
    case QEvent::Leave:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::ToolTip:
    case QEvent::Resize:
    case QEvent::DragEnter:
    case QEvent::ContextMenu:
    case QEvent::Wheel:
        break;
    default:
        return false;
    }
    View* v = viewForViewport(watched);
    if (!v || !v->browser)
        return false;
    QTextBrowser* browser = v->browser;
    switch (type) {
    case QEvent::Paint: {
        if (m_mode == Mode::Classic)
            return false;
        if (v->document != browser->document())
            return false;
        // Before the chat paints: the pictures it is about to show, then what goes under its text.
        renderVisible(*v);
        if (!v->overlay) {
            QPainter p(browser->viewport());
            paintLayer(*v, p, static_cast<QPaintEvent*>(event)->region(), false);
        }
        return false;
    }
    case QEvent::Resize:
        if (v->overlayWidget)
            v->overlayWidget->setGeometry(browser->viewport()->rect());
        schedule(0); // a new width (applied once it settles)
        return false;
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->buttons() & Qt::LeftButton) {
            hideBar(false); // a selection going on
            return false;
        }
        updateHover(*v, me->pos(), true);
        if (v->replyHover || v->chipHover >= 0)
            return true; // our cursor, not QTextBrowser's link handling (the head is a link)
        return false;
    }
    case QEvent::Leave:
        if (!(m_bar && m_bar->isVisible() && m_bar->underMouse()))
            clearHover(*v);
        return false;
    case QEvent::Wheel:
        return false;
    case QEvent::DragEnter:
        hideBar(false);
        return false;
    case QEvent::ContextMenu:
        hideBar(false);
        return false;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto* me = static_cast<QMouseEvent*>(event);
        hideBar(false);
        int block = -1;
        if (me->button() == Qt::LeftButton && type == QEvent::MouseButtonPress) {
            const int chip = chipAt(*v, me->pos());
            if (chip >= 0) {
                v->chipPressed = true;
                toggleRun(*v, chip); // P2: a run of events opens or closes
                return true;
            }
        }
        if (me->button() == Qt::LeftButton && replyRowAt(*v, me->pos(), &block)) {
            // The reply row: taken before TeamSpeak's link click on the head.
            v->replyPressed    = true;
            ++v->renderStamp;
            v->replyPressBlock = block;
            browser->viewport()->update(slotOf(*v, block));
            return true;
        }
        return false;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && v->chipPressed) {
            v->chipPressed = false;
            return true; // the chip's (TeamSpeak saw no press either)
        }
        if (me->button() != Qt::LeftButton || !v->replyPressed)
            return false;
        const int pressed  = v->replyPressBlock;
        v->replyPressed    = false;
        ++v->renderStamp;
        v->replyPressBlock = -1;
        browser->viewport()->update(slotOf(*v, pressed));
        int block = -1;
        if (replyRowAt(*v, me->pos(), &block) && block == pressed && m_chat->m_replies)
            m_chat->m_replies->activateReplyOf(browser, block); // jumps to the original and flashes it
        return true;
    }
    case QEvent::ToolTip:
        return m_mode != Mode::Classic && toolTip(*v, event);
    default:
        break;
    }
    return false;
}

bool ChatLayout::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_bar.data()) {
        if (event->type() == QEvent::Leave) {
            // Off the bar: back over the row, or out of the chat.
            if (View* v = viewFor(m_barIn.data())) {
                QWidget* vp = v->browser->viewport();
                if (vp->rect().contains(vp->mapFromGlobal(QCursor::pos())))
                    updateHover(*v, vp->mapFromGlobal(QCursor::pos()), true);
                else
                    clearHover(*v);
            }
        }
        return false;
    }
    if (event->type() == QEvent::ContextMenu) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        if (ce->reason() == QContextMenuEvent::Keyboard) {
            for (auto it = m_views.begin(); it != m_views.end(); ++it) {
                if (it->browser.data() == watched) {
                    keyboardMenu(it->browser.data());
                    break;
                }
            }
        }
    }
    return false;
}

QString ChatLayout::describe(QTextBrowser* browser) const
{
    auto it = m_views.constFind(browser);
    if (it == m_views.constEnd() || !browser)
        return QString::fromLatin1("layout: not attached");
    QTextDocument* doc = browser->document();
    int            kinds[9] = {0};
    int            tagged   = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const layoutformat::Lead lead = layoutformat::leadOf(b);
        if (lead.styled)
            ++kinds[qBound(0, static_cast<int>(lead.kind), 8)];
        tagged += b.blockFormat().hasProperty(layoutformat::kBlockTag) ? 1 : 0;
    }
    return QString::fromLatin1("layout: mode %1, %2 blocks, %3 tagged, heads %4, continuations %5, compact heads %6, system %7, dividers %8, epoch %9, overlay %10, "
                               "undo %11, f %12, G %13, L %14, pictures %15, history %16")
        .arg(static_cast<int>(m_mode))
        .arg(doc->blockCount())
        .arg(tagged)
        .arg(kinds[layoutformat::Head])
        .arg(kinds[layoutformat::Continuation])
        .arg(kinds[layoutformat::CompactHead])
        .arg(kinds[layoutformat::SystemPrefix])
        .arg(kinds[layoutformat::Divider])
        .arg(it->epoch)
        .arg(it->overlay ? 1 : 0)
        .arg(doc->isUndoRedoEnabled() ? 1 : 0)
        .arg(it->env.tokens.f)
        .arg(it->env.tokens.G)
        .arg(it->env.tokens.L)
        .arg(it->rendered.size())
        .arg(it->historyNext);
}
