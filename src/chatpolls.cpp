#include "chatpolls.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHelpEvent>
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
#include <QUrl>
#include <QtMath>

#include <algorithm>

#include <QJsonArray>
#include <QJsonObject>

#include "attachart.h"
#include "attachbutton.h"
#include "attachmenu.h"
#include "chatintegration.h"
#include "chatlayout.h"
#include "chatreplies.h"
#include "chatscroll.h"
#include "emojitext.h"
#include "i18n.h"
#include "layoutformat.h"
#include "medialink.h"
#include "peerhub.h"
#include "pluginlink.h"
#include "polldialog.h"
#include "polldoc.h"
#include "pollhub.h"
#include "settings.h"
#include "ts3api.h"
#include "uiutil.h"

namespace {

constexpr int kGapTop    = 4; // transparent room above and below the card in its picture
constexpr int kGapBottom = 4;
constexpr int kSyncEveryMs = 4000; // the polls on screen are offered to PollHub (it limits itself)

QString entryId()
{
    return QString::fromLatin1("poll");
}

bool atBottom(QTextBrowser* b)
{
    const QScrollBar* sb = b->verticalScrollBar();
    return sb->value() >= sb->maximum() - 4;
}

// The picture of a card that isn't on screen: 1 x 1 transparent (never missing: Qt would draw its file icon).
QPixmap blankPicture()
{
    QPixmap pm(1, 1);
    pm.fill(Qt::transparent);
    return pm;
}

qint64 monotonicMs()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return clock.elapsed();
}

} // namespace

// ---- TeamSpeak and the transport for PollHub ----------------------------------------------------------------

class ChatPolls::HubEnv : public PollHub::Env
{
  public:
    qint64  nowMs() const override { return monotonicMs(); }
    qint64  wallMs() const override { return QDateTime::currentMSecsSinceEpoch(); }
    bool    isConnected(quint64 sch) const override { return ts3::isConnected(sch); }
    quint64 ownChannel(quint64 sch) const override { return ts3::isConnected(sch) ? ts3::ownChannel(sch) : 0; }

    quint64 channelOf(quint64 sch, quint16 client) const override
    {
        uint64 channel = 0;
        if (!client || !ts3::funcs.getChannelOfClient || ts3::funcs.getChannelOfClient(sch, client, &channel) != ERROR_ok)
            return 0;
        return channel;
    }

    bool isQueryClient(quint64 sch, quint16 client) const override
    {
        int type = 0;
        return ts3::funcs.getClientVariableAsInt && ts3::funcs.getClientVariableAsInt(sch, client, CLIENT_TYPE, &type) == ERROR_ok
               && type == ClientType_SERVERQUERY;
    }

    bool othersHaveTsMedia(quint64 sch) const override
    {
        PeerHub* hub = PeerHub::instance();
        if (!hub)
            return false;
        peers::PeerDirectory* d = hub->directory();
        return d->channelHasCapability(sch, "r") || d->channelHasCapability(sch, "p");
    }

    bool blocked(quint64 sch) const override
    {
        PeerHub* hub = PeerHub::instance();
        return !hub || hub->link()->blocked(sch);
    }

    // Presence off (or no plugin commands there): no questions, no answers; the user's own votes only.
    bool quiet(quint64 sch) const override
    {
        PeerHub* hub = PeerHub::instance();
        return !hub || !hub->directory()->isAvailable(sch);
    }

    int randomBetween(int low, int high) override { return low >= high ? low : QRandomGenerator::global()->bounded(low, high + 1); }

    void send(quint64 sch, const proto::Message& message, bool background, const QString& coalesce, peers::Sender::Done done) override
    {
        PeerHub* hub = PeerHub::instance();
        if (!hub) {
            if (done)
                done(peers::SendResult::Failed);
            return;
        }
        hub->link()->sendWith(sch, message, peers::Target::toChannel(), background ? PluginLink::Priority::Background : PluginLink::Priority::Reaction,
                              std::move(done), coalesce);
    }
};

// ---- the module -----------------------------------------------------------------------------------------------

ChatPolls::ChatPolls(ChatIntegration* chat)
    : QObject(chat)
    , m_chat(chat)
    , m_env(std::make_unique<HubEnv>())
{
    m_tag = QString::number(QRandomGenerator::global()->generate() & 0xffffff, 16) + QLatin1Char('-');
    m_hub = new PollHub(*m_env, ts3::dataDir().isEmpty() ? QString() : ts3::dataDir() + QLatin1String("/polls.json"), this);
    connect(m_hub, &PollHub::changed, this, &ChatPolls::keyChanged);
    if (PeerHub* peers = PeerHub::instance()) {
        connect(peers->link(), &PluginLink::received, m_hub, &PollHub::received);
        // Entering a channel with TS Media people: the polls on screen are asked about.
        connect(peers->directory(), &peers::PeerDirectory::syncDue, this, [this](quint64) { syncVisible(); });
    }
    // TS Media's layout styled blocks (or gave them back): the cards' line breaks follow the new look.
    if (chat->m_layout) {
        connect(chat->m_layout, &ChatLayout::documentEdited, this, [this](QTextBrowser* browser) {
            View* view = viewOf(browser);
            if (!view || view->cards.isEmpty() || view->document != browser->document())
                return;
            // The reader's place across our edit (live test 2.3: a layout switch at the bottom of a chat with
            // cards left it about a card's line above the end; the layout had already put it back).
            const chatscroll::Place place       = chatscroll::capture(browser);
            const bool              wasMutating = m_chat->m_mutating; // our own edit: no rescan of the chat for it
            m_chat->m_mutating                  = true;
            const int changed                   = polldoc::normalizeAll(view->document);
            m_chat->m_mutating                  = wasMutating;
            if (changed > 0) {
                chatscroll::restore(browser, place);
#ifdef TSMEDIA_TESTHOOKS
                ts3::log(QString::fromLatin1("[test] polls: %1 card line(s) changed after a layout edit; place kept (bottom %2)").arg(changed).arg(place.bottom ? 1 : 0));
#endif
                view->positionsValid = false;
                if (m_chat->m_replies)
                    m_chat->m_replies->documentEdited(browser);
            }
        });
    }
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(kSyncEveryMs);
    connect(m_syncTimer, &QTimer::timeout, this, &ChatPolls::syncVisible);
    m_syncTimer->start();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(0);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        const QList<QTextBrowser*> browsers = m_refreshWanted.keys();
        m_refreshWanted.clear();
        beginPass();
        for (QTextBrowser* b : browsers) {
            if (m_views.contains(b))
                refreshSizes(b);
        }
    });

    // The attach menu's Poll (attachmenu.h: order 30).
    const QPointer<ChatPolls> self(this);
    attachmenu::Entry entry;
    entry.id      = entryId();
    entry.order   = 30;
    entry.text    = [] { return i18n::t("Poll"); };
    entry.icon    = [] { return attachart::icon(attachart::Glyph::Poll); };
    entry.enabled = [] { return ts3::isConnected(ts3::currentConnection()); };
    entry.trigger = [self](QWidget* anchor) {
        if (self)
            self->openCreator(anchor ? anchor->parentWidget() : nullptr);
    };
    attachmenu::addEntry(entry);
}

ChatPolls::~ChatPolls()
{
    attachmenu::removeEntry(entryId());
    if (m_dialog)
        delete m_dialog.data();
    if (m_cursorSet && m_hover.browser)
        m_hover.browser->viewport()->setCursor(m_savedCursor); // the chat's own cursor back
    restoreAll();
    m_hub->prepareShutdown(); // polls.json written, nothing more is sent
    // Now, while m_env (its Env) is still there: as a child it would go only after our members.
    delete m_hub;
    m_hub = nullptr;
}

void ChatPolls::restoreAll()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (!it->document)
            continue;
        layoutformat::MutatingScope mutating; // ChatIntegration is going (its flag is set too)
        polldoc::giveBack(it->document.data());
    }
    m_views.clear();
}

ChatPolls::View* ChatPolls::viewOf(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

void ChatPolls::track(QTextBrowser* browser, View& view)
{
    if (view.tracked)
        return;
    view.tracked = true;
    connect(browser, &QObject::destroyed, this, [this, browser] {
        m_views.remove(browser);
        m_refreshWanted.remove(browser);
    });
}

QString ChatPolls::newObjectName()
{
    return polldoc::objectPrefix() + m_tag + QString::number(++m_nextObject);
}

int ChatPolls::cardCount(QTextBrowser* browser) const
{
    const auto it = m_views.constFind(browser);
    return it == m_views.constEnd() ? 0 : it->cards.size();
}

QString ChatPolls::describeCards(QTextBrowser* browser) const
{
    QStringList lines;
    const auto  it = m_views.constFind(browser);
    if (it == m_views.constEnd())
        return {};
    for (const Card& card : it->cards) {
        const PollHub::View v = m_hub->view(card.poll);
        lines << QString::fromLatin1("%1 options=%2 voters=%3 own=%4").arg(card.poll.key).arg(card.poll.options.size()).arg(v.tally.voters).arg(v.own);
    }
    lines.sort();
    return lines.join(QLatin1Char('\n'));
}

// ---- reading the chat ---------------------------------------------------------------------------------------------

void ChatPolls::afterScan(QTextBrowser* browser)
{
    if (!browser)
        return;
    beginPass();
    View& view = m_views[browser];
    view.browser = browser;
    track(browser, view);
    QTextDocument* doc = browser->document();
    if (view.document != doc) {
        // TeamSpeak swapped the chat's document: the old one gets its text back (it may be shown again).
        if (view.document) {
            layoutformat::MutatingScope mutating;
            polldoc::giveBack(view.document.data());
        }
        view.document = doc;
        view.cards.clear();
        view.positionsValid = false;
        connect(doc, &QTextDocument::contentsChange, this, [this, browser, guard = QPointer<QTextDocument>(doc)](int, int, int) {
            View* v = viewOf(browser);
            if (v && v->document == guard)
                v->positionsValid = false;
        });
    }

    const QVector<polldoc::Found> found = polldoc::scan(doc);
    if (found.isEmpty() && view.cards.isEmpty())
        return;
    QHash<QString, Card> cards;
    QVector<int>         raw;
    for (int i = 0; i < found.size(); ++i) {
        const polldoc::Found& f = found.at(i);
        if (!f.collapsed) {
            raw.append(i);
            continue;
        }
        Card card = view.cards.value(f.object);
        card.poll   = f.poll;
        card.object = f.object;
        cards.insert(f.object, card);
    }
    bool edited = false;
    {
        // Our own edits: ChatIntegration doesn't read the chat again for them, and the layout leaves them
        // alone (like the replies' edits); the replies hear of them below. One edit block: the chat is laid
        // out again once, not once per poll (a reloaded history).
        const bool wasMutating = m_chat->m_mutating;
        m_chat->m_mutating     = true;
        const bool  bottom     = !raw.isEmpty() && atBottom(browser);
        QTextCursor batch(doc);
        batch.beginEditBlock();
        // Back to front: earlier positions stay valid.
        for (int k = raw.size() - 1; k >= 0; --k) {
            const polldoc::Found& f = found.at(raw.at(k));
            Card                  card;
            card.poll   = f.poll;
            card.object = newObjectName();
            const QSize size = objectSize(browser, card);
            // An empty picture until it is drawn on screen (right before the chat paints).
            doc->addResource(QTextDocument::ImageResource, QUrl(card.object), blankPicture());
            if (polldoc::collapse(doc, f, card.object, size)) {
                cards.insert(card.object, card);
                edited = true;
            }
        }
        if (!cards.isEmpty() && polldoc::normalizeAll(doc) > 0)
            edited = true;
        batch.endEditBlock(); // TeamSpeak's view hears of it while we are still "mutating"
        m_chat->m_mutating = wasMutating;
        if (bottom)
            browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
    }
    view.cards          = cards;
    view.positionsValid = false;
    // Only ballots of polls a chat shows are taken (PollHub::watch).
    for (const Card& card : qAsConst(view.cards))
        m_hub->watch(card.poll);
    if (edited) {
        // The replies' index of this chat: positions inside the blocks moved.
        if (m_chat->m_replies)
            m_chat->m_replies->documentEdited(browser);
        browser->viewport()->update();
    }
    if (browser->isVisible())
        syncVisible();
}

void ChatPolls::ensurePositions(View& view)
{
    if (view.positionsValid || !view.document)
        return;
    view.positions.clear();
    QTextDocument* doc = view.document;
    for (const int p : polldoc::objectPositions(doc)) {
        QTextCursor c(doc);
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        view.positions.insert(c.charFormat().toImageFormat().name(), p);
    }
    view.positionsValid = true;
}

void ChatPolls::beginPass() const
{
    m_memo = Memo(); // the chat's target and look are read again (once per event, not once per card)
}

bool ChatPolls::targetOf(QTextBrowser* browser, ChatTarget* target) const
{
    // Only the chat that is shown can be told apart (its tab is the current one).
    if (!browser || !browser->isVisible())
        return false;
    if (!m_memo.targetKnown || m_memo.targetBrowser != browser) {
        m_memo.targetBrowser = browser;
        m_memo.targetKnown   = true;
        m_memo.hasTarget     = m_chat->resolveTarget(browser, &m_memo.target);
    }
    *target = m_memo.target;
    return m_memo.hasTarget;
}

// ---- drawing ------------------------------------------------------------------------------------------------------

pollart::Style ChatPolls::styleOf(QTextBrowser* browser) const
{
    if (m_memo.styleKnown && m_memo.styleBrowser == browser)
        return m_memo.style;
    m_memo.style        = computeStyle(browser);
    m_memo.styleBrowser = browser;
    m_memo.styleKnown   = true;
    return m_memo.style;
}

pollart::Style ChatPolls::computeStyle(QTextBrowser* browser) const
{
    const PreviewStyle preview = m_chat->styleFor(browser);
    pollart::Style     style;
    style.dark = preview.dark;
    style.font = preview.font;
    style.dpr  = preview.dpr;
    style.base = browser->viewport()->palette().color(QPalette::Base);
    if (style.dark != (style.base.lightness() < 128))
        style.base = QColor(); // a style sheet colours the chat: the theme's default
    style.text = browser->palette().color(QPalette::Text);
    if (style.base.isValid() && ui::contrastRatio(style.text, style.base) < 4.5)
        style.text = QColor();
    else if (!style.base.isValid() && style.dark != (style.text.lightness() > 128))
        style.text = QColor();
    // The layout's content column (Cozy / Compact indent), within the chat's margins; pollart keeps it at 400.
    const int indent = m_chat->m_layout ? m_chat->m_layout->indentFor(browser) : 0;
    const int margin = static_cast<int>(2 * browser->document()->documentMargin());
    // 2.3: in a bubble, its right margin too (the card stays inside the bubble).
    const int right  = qMax(12, m_chat->m_layout ? m_chat->m_layout->bubbleRightFor(browser) + 2 : 0);
    style.maxWidth   = qMax(160, m_chat->previewAreaWidth(browser) - indent - margin - right);
    style.bubbles    = preview.bubbles; // 2.3: in a message bubble the card has no panel of its own
    return style;
}

pollart::Card ChatPolls::stateOf(QTextBrowser* browser, const Card& card) const
{
    pollart::Card       c;
    const polls::Poll&  poll = card.poll;
    const PollHub::View v    = m_hub->view(poll);
    c.question               = poll.question;
    c.options                = poll.options;
    c.multiple               = poll.multiple;
    c.results                = v.own != 0;
    c.counts                 = v.tally.counts;
    c.percents               = v.percents;
    c.voters                 = v.tally.voters;
    c.own                    = v.own;
    c.selected               = c.results ? 0 : card.selected;
    c.sending                = v.state == PollHub::SendState::Sending;
    c.failed                 = v.state == PollHub::SendState::Failed;
    c.locked                 = v.locked;
    ChatTarget target;
    if (targetOf(browser, &target)) {
        if (target.mode != TextMessageTarget_CHANNEL) {
            c.canVote = false;
            c.note    = i18n::t("Polls are voted on in the channel chat.");
        } else {
            const PollHub::VoteError error = m_hub->canVote(target.sch, poll);
            c.canVote                      = error == PollHub::VoteError::None;
            if (!c.canVote)
                c.note = PollHub::errorText(error);
        }
    } else {
        c.canVote = false; // not on screen: drawn again before it is
    }
    if (c.results)
        c.note.clear(); // you voted: the results say enough
    return c;
}

pollart::Pointer ChatPolls::pointerFor(QTextBrowser* browser, const QString& object) const
{
    pollart::Pointer pointer;
    if (m_hover.browser == browser && m_hover.object == object) {
        pointer.hover      = m_hover.zone;
        pointer.hoverIndex = m_hover.index;
    }
    if (m_press.browser == browser && m_press.object == object) {
        pointer.pressed      = m_press.zone;
        pointer.pressedIndex = m_press.index;
    }
    return pointer;
}

QSize ChatPolls::objectSize(QTextBrowser* browser, const Card& card) const
{
    const pollart::Layout l = pollart::layout(stateOf(browser, card), styleOf(browser));
    return QSize(l.size.width(), l.size.height() + kGapTop + kGapBottom);
}

QString ChatPolls::signatureOf(QTextBrowser* browser, const Card& card) const
{
    const pollart::Card    c = stateOf(browser, card);
    const pollart::Style   s = styleOf(browser);
    const pollart::Pointer p = pointerFor(browser, card.object);
    QStringList            parts;
    parts << QString::number(s.dark) << s.base.name() << s.text.name() << s.font.toString() << QString::number(s.dpr) << QString::number(s.maxWidth)
          << (s.bubbles.isEmpty() ? QString() : s.bubbles.first().name()) // 2.3: on a bubble
          << QString::number(emoji::textPicturesEnabled()) << QString::number(c.results) << QString::number(c.voters) << QString::number(c.own)
          << QString::number(c.selected) << QString::number(c.canVote) << c.note << QString::number(c.sending) << QString::number(c.failed)
          << QString::number(c.locked) << QString::number(static_cast<int>(p.hover)) << QString::number(p.hoverIndex)
          << QString::number(static_cast<int>(p.pressed)) << QString::number(p.pressedIndex);
    for (const int n : c.counts)
        parts << QString::number(n);
    return parts.join(QLatin1Char('|'));
}

void ChatPolls::render(QTextBrowser* browser, View& view, Card& card)
{
    const pollart::Card  c     = stateOf(browser, card);
    const pollart::Style style = styleOf(browser);
    pollart::Layout      l;
    const QImage         panel = pollart::render(c, style, pointerFor(browser, card.object), &l);
    const qreal          dpr   = qMax<qreal>(1.0, style.dpr);
    QImage               padded(QSize(qCeil(panel.width()), qCeil(panel.height() + (kGapTop + kGapBottom) * dpr)), QImage::Format_ARGB32_Premultiplied);
    padded.setDevicePixelRatio(dpr);
    padded.fill(Qt::transparent);
    {
        QPainter p(&padded);
        p.drawImage(QPointF(0, kGapTop), panel);
    }
    view.document->addResource(QTextDocument::ImageResource, QUrl(card.object), QPixmap::fromImage(padded));
    card.rendered = signatureOf(browser, card);
    if (emoji::takeTextFallbacks() > 0) {
        card.rendered.clear(); // some emoji are still on their way: drawn again soon
        scheduleRefresh(browser);
    }
}

QRectF ChatPolls::cardRect(QTextBrowser* browser, int position, const QSizeF& size) const
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

void ChatPolls::renderVisible(QTextBrowser* browser)
{
    View* view = viewOf(browser);
    if (!view || view->cards.isEmpty() || view->document != browser->document())
        return;
    ensurePositions(*view);
    const QRectF shown = QRectF(browser->viewport()->rect()).adjusted(0, -400, 0, 400);
    const QRectF far   = QRectF(browser->viewport()->rect()).adjusted(0, -3000, 0, 3000);
    bool         resize = false;
    for (auto it = view->cards.begin(); it != view->cards.end(); ++it) {
        Card&     card     = it.value();
        const int position = view->positions.value(card.object, -1);
        if (position < 0)
            continue;
        QTextCursor c(view->document);
        c.setPosition(position);
        c.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat format = c.charFormat().toImageFormat();
        const QSizeF           size(format.width(), format.height());
        const QRectF           rect = cardRect(browser, position, size);
        if (rect.intersects(shown)) {
            if (objectSize(browser, card) != size.toSize()) {
                resize = true; // laid out again first (not while the chat paints)
                continue;
            }
            if (card.rendered != signatureOf(browser, card))
                render(browser, *view, card);
        } else if (!card.rendered.isEmpty() && !rect.intersects(far)) {
            // Far off screen: its memory goes (drawn again when it comes back).
            view->document->addResource(QTextDocument::ImageResource, QUrl(card.object), blankPicture());
            card.rendered.clear();
        }
    }
    if (resize)
        scheduleRefresh(browser);
}

void ChatPolls::scheduleRefresh(QTextBrowser* browser)
{
    m_refreshWanted.insert(browser, true);
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
}

void ChatPolls::refreshSizes(QTextBrowser* browser)
{
    View* view = viewOf(browser);
    if (!view || view->document != browser->document())
        return;
    ensurePositions(*view);
    struct Change {
        int    position;
        QSizeF size;
    };
    QVector<Change> changes;
    for (const Card& card : qAsConst(view->cards)) {
        const int position = view->positions.value(card.object, -1);
        if (position < 0)
            continue;
        QTextCursor c(view->document);
        c.setPosition(position);
        c.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat format = c.charFormat().toImageFormat();
        const QSize            want   = objectSize(browser, card);
        if (QSizeF(format.width(), format.height()).toSize() != want)
            changes.append({position, QSizeF(want)});
    }
    if (!changes.isEmpty()) {
        const bool                  bottom = atBottom(browser);
        layoutformat::MutatingScope mutating; // a format change of our own picture: nobody needs to read the chat again
        QTextCursor                 batch(view->document);
        batch.beginEditBlock();
        for (const Change& change : qAsConst(changes))
            polldoc::resizeObject(view->document, change.position, change.size);
        batch.endEditBlock();
        if (bottom)
            browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
    }
    browser->viewport()->update();
}

void ChatPolls::repaintCard(QTextBrowser* browser, const QString& object)
{
    View* view = viewOf(browser);
    if (!view)
        return;
    auto it = view->cards.find(object);
    if (it == view->cards.end())
        return;
    ensurePositions(*view);
    const int position = view->positions.value(object, -1);
    if (position < 0)
        return;
    render(browser, *view, it.value());
    QTextCursor c(view->document);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    const QTextImageFormat format = c.charFormat().toImageFormat();
    browser->viewport()->update(cardRect(browser, position, QSizeF(format.width(), format.height())).toAlignedRect().adjusted(-2, -6, 2, 6));
}

void ChatPolls::keyChanged(const QString& key)
{
    beginPass();
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        bool has = false;
        for (const Card& card : qAsConst(it->cards))
            has = has || card.poll.key == key;
        if (has && it->browser) {
            if (it->browser->isVisible())
                refreshSizes(it->browser);
            else
                it->browser->viewport()->update();
        }
    }
}

// ---- the pointer ---------------------------------------------------------------------------------------------------

ChatPolls::Spot ChatPolls::spotAt(QTextBrowser* browser, const QPoint& pos, QRectF* rectOut)
{
    Spot  spot;
    View* view = viewOf(browser);
    if (!view || view->cards.isEmpty() || view->document != browser->document())
        return spot;
    ensurePositions(*view);
    for (auto it = view->cards.constBegin(); it != view->cards.constEnd(); ++it) {
        const int position = view->positions.value(it.key(), -1);
        if (position < 0)
            continue;
        QTextCursor c(view->document);
        c.setPosition(position);
        c.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat format = c.charFormat().toImageFormat();
        const QRectF           rect   = cardRect(browser, position, QSizeF(format.width(), format.height()));
        if (!rect.contains(pos))
            continue;
        const pollart::Card   state  = stateOf(browser, it.value());
        const pollart::Layout layout = pollart::layout(state, styleOf(browser));
        const pollart::Hit    hit    = pollart::hitTest(state, layout, QPointF(pos) - rect.topLeft());
        spot.browser                 = browser;
        spot.object                  = it.key();
        spot.zone                    = hit.zone;
        spot.index                   = hit.index;
        if (rectOut)
            *rectOut = rect;
        return spot;
    }
    return spot;
}

void ChatPolls::setHover(const Spot& spot)
{
    if (spot.same(m_hover))
        return;
    const Spot old = m_hover;
    m_hover        = spot;
    if (old.browser && !old.object.isEmpty())
        repaintCard(old.browser, old.object);
    if (spot.browser && !spot.object.isEmpty() && spot.object != old.object)
        repaintCard(spot.browser, spot.object);
    else if (spot.browser && !spot.object.isEmpty() && old.browser != spot.browser)
        repaintCard(spot.browser, spot.object);
    if (spot.object.isEmpty() && old.browser)
        applyCursor(old.browser, spot);
}

// Our cursor while on a card: a pointing hand on what a click does, the arrow elsewhere on it; the chat's
// own cursor back when the pointer leaves the card.
void ChatPolls::applyCursor(QTextBrowser* browser, const Spot& spot)
{
    if (!browser)
        return;
    QWidget*   vp         = browser->viewport();
    const bool actionable = spot.zone == pollart::Zone::Option || spot.zone == pollart::Zone::Submit || spot.zone == pollart::Zone::Retract
                            || spot.zone == pollart::Zone::Retry;
    if (!spot.object.isEmpty()) {
        if (!m_cursorSet) {
            m_savedCursor = vp->cursor().shape() == Qt::PointingHandCursor ? Qt::ArrowCursor : vp->cursor().shape();
            m_cursorSet   = true;
        }
        const Qt::CursorShape shape = actionable ? Qt::PointingHandCursor : Qt::ArrowCursor;
        if (vp->cursor().shape() != shape)
            vp->setCursor(shape);
    } else if (m_cursorSet) {
        vp->setCursor(m_savedCursor);
        m_cursorSet = false;
    }
}

bool ChatPolls::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::Paint && type != QEvent::MouseMove && type != QEvent::Leave && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
        && type != QEvent::MouseButtonDblClick && type != QEvent::ToolTip && type != QEvent::Resize)
        return false;
    QTextBrowser* browser = nullptr;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && it->browser->viewport() == watched)
            browser = it->browser;
    }
    if (!browser)
        return false;
    beginPass();
    switch (type) {
    case QEvent::Paint:
        renderVisible(browser);
        return false;
    case QEvent::Resize:
        scheduleRefresh(browser); // another width
        return false;
    case QEvent::Leave:
        setHover(Spot());
        return false;
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->buttons() & Qt::LeftButton) {
            if (m_press.browser == browser)
                return true; // a press on a card: no text selection from there
            return false;
        }
        const Spot spot     = spotAt(browser, me->pos());
        const bool entering = !spot.object.isEmpty() && m_hover.object.isEmpty();
        // Onto a card: the chat's own hover (a preview's look, video controls) ends, as it would on a move
        // it saw (the card takes the moves from here on).
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
        if (spot.object.isEmpty() || me->button() != Qt::LeftButton)
            return false; // the right button: the chat's menus as usual
        if (spot.zone == pollart::Zone::None || spot.zone == pollart::Zone::Badge)
            return true; // the card's body: nothing (no text selection, no link)
        m_press = spot;
        repaintCard(browser, spot.object);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || m_press.object.isEmpty())
            return false;
        const Spot pressed = m_press;
        m_press            = Spot();
        const Spot spot    = spotAt(browser, me->pos());
        if (pressed.browser == browser)
            repaintCard(browser, pressed.object);
        if (spot.same(pressed))
            activate(browser, spot);
        return true;
    }
    case QEvent::ToolTip: {
        auto*      help = static_cast<QHelpEvent*>(event);
        QRectF     rect;
        const Spot spot = spotAt(browser, help->pos(), &rect);
        if (spot.object.isEmpty())
            return false;
        QString tip;
        switch (spot.zone) {
        case pollart::Zone::Badge:
            tip = pollart::badgeToolTip();
            break;
        case pollart::Zone::Retract:
            tip = i18n::t("Take your vote back (you can vote again)");
            break;
        case pollart::Zone::Retry:
            tip = i18n::t("Send your vote again");
            break;
        default:
            break;
        }
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

void ChatPolls::activate(QTextBrowser* browser, const Spot& spot)
{
    View* view = viewOf(browser);
    if (!view)
        return;
    auto it = view->cards.find(spot.object);
    if (it == view->cards.end())
        return;
    Card&      card = it.value();
    ChatTarget target;
    if (!targetOf(browser, &target))
        return;
    PollHub::VoteError error = PollHub::VoteError::None;
    switch (spot.zone) {
    case pollart::Zone::Option:
        if (card.poll.multiple) {
            card.selected ^= static_cast<quint16>(1u << spot.index);
            repaintCard(browser, card.object);
            return;
        }
        error = m_hub->vote(target.sch, card.poll, static_cast<quint16>(1u << spot.index));
        break;
    case pollart::Zone::Submit:
        error = m_hub->vote(target.sch, card.poll, card.selected);
        if (error == PollHub::VoteError::None)
            card.selected = 0;
        break;
    case pollart::Zone::Retract:
        error = m_hub->vote(target.sch, card.poll, 0);
        break;
    case pollart::Zone::Retry:
        m_hub->retry(target.sch, card.poll);
        break;
    default:
        return;
    }
    if (error != PollHub::VoteError::None)
        m_chat->showFeedback(browser->viewport(), PollHub::errorText(error), true);
    refreshSizes(browser); // the results take more room than the options
}

void ChatPolls::syncVisible()
{
    beginPass();
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        QTextBrowser* browser = it->browser;
        if (!browser || !browser->isVisible() || it->cards.isEmpty())
            continue;
        ChatTarget target;
        if (!targetOf(browser, &target) || target.mode != TextMessageTarget_CHANNEL || !ts3::isConnected(target.sch))
            continue;
        const quint64 channel = ts3::ownChannel(target.sch);
        QStringList   keys;
        for (const Card& card : it->cards) {
            if (card.poll.channelId == channel && !keys.contains(card.poll.key))
                keys << card.poll.key;
        }
        if (!keys.isEmpty())
            m_hub->requestSync(target.sch, keys);
    }
}

// ---- making a poll -----------------------------------------------------------------------------------------------

QString ChatPolls::messageFor(const polls::Poll& poll, quint64 sch)
{
    const Settings s    = Settings::instance().forServer(ts3::serverUid(sch));
    const QString  text = polls::composeMessage(poll, s.pluginDownloadUrl);
    return escapedMessageSize(text) <= kMaxMessageBytes ? text : QString();
}

void ChatPolls::openCreator(QWidget* source)
{
    ChatTarget target;
    QWidget*   from = source;
    if (!from || !m_chat->resolveTarget(from, &target))
        m_chat->resolveCurrentTarget(&target, &from);
    QWidget* feedback = from ? from : m_chat->mainWindow();
    if (!ts3::isConnected(target.sch)) {
        m_chat->showFeedback(feedback, i18n::t("Connect to a server to post a poll."), true);
        return;
    }
    if (target.mode != TextMessageTarget_CHANNEL) {
        m_chat->showFeedback(feedback, i18n::t("Polls can be posted in the channel chat only."), true);
        return;
    }
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    const quint64 sch    = target.sch;
    auto*         dialog = new PollDialog(m_chat->describeTarget(target), m_chat->mainWindow());
    m_dialog             = dialog;
    dialog->setValidator([sch](const PollDialog::Result& r) -> QString {
        polls::Poll poll;
        poll.key       = QString::fromLatin1("ffffffffffffffff");
        poll.question  = r.question;
        poll.options   = r.options;
        poll.multiple  = r.multiple;
        poll.channelId = 999999999999999ULL; // the longest channel id
        if (!ts3::isConnected(sch))
            return i18n::t("You're not connected to this server any more.");
        return messageFor(poll, sch).isEmpty() ? i18n::t("The poll is too long for one chat message. Shorten the question or the options.") : QString();
    });
    const QPointer<ChatPolls> self(this);
    connect(dialog, &QDialog::accepted, this, [self, dialog, sch] {
        if (!self)
            return;
        const PollDialog::Result r       = dialog->result();
        const QString            problem = self->post(sch, r.question, r.options, r.multiple);
        if (!problem.isEmpty())
            self->m_chat->showFeedback(self->m_chat->mainWindow(), problem, true);
    });
    dialog->open();
}

QString ChatPolls::post(quint64 sch, const QString& question, const QStringList& options, bool multiple)
{
    polls::Poll poll;
    poll.key      = polls::newKey();
    poll.question = polls::cleanLine(question, polls::kMaxQuestionChars);
    for (const QString& option : options) {
        const QString clean = polls::cleanLine(option, polls::kMaxOptionChars);
        if (!clean.isEmpty() && poll.options.size() < polls::kMaxOptions)
            poll.options << clean;
    }
    poll.multiple  = multiple;
    poll.channelId = ts3::isConnected(sch) ? ts3::ownChannel(sch) : 0;
    const QString text = messageFor(poll, sch);
    if (!poll.isValid() || text.isEmpty() || !ts3::funcs.requestSendChannelTextMsg)
        return i18n::t("The poll couldn't be posted.");
    // Typed by you, like a chat message: sent at once (TeamSpeak's own flood rules apply to it).
    const QByteArray   utf8  = text.toUtf8();
    const unsigned int error = ts3::funcs.requestSendChannelTextMsg(sch, utf8.constData(), poll.channelId, nullptr);
    if (error != ERROR_ok)
        return i18n::t("The poll couldn't be posted: %1").arg(ts3::errorText(error));
    return {};
}

#ifdef TSMEDIA_TESTHOOKS
QString ChatPolls::testCommand(const QString& cmd, const QJsonObject& c)
{
    beginPass();
    const uint64  sch     = ts3::currentConnection();
    QTextBrowser* browser = m_chat->visibleChatBrowser();
    if (cmd == QLatin1String("poll.post")) {
        QStringList options;
        for (const QJsonValue& v : c.value(QString::fromLatin1("options")).toArray())
            options << v.toString();
        const QString to = c.value(QString::fromLatin1("to")).toString();
        if (!to.isEmpty()) {
            // Live pass A: the same message as a private message to a client of this server (a poll in a
            // private chat is not voted on there; the card says so).
            polls::Poll poll;
            poll.key      = polls::newKey();
            poll.question = polls::cleanLine(c.value(QString::fromLatin1("question")).toString(), polls::kMaxQuestionChars);
            for (const QString& option : options)
                poll.options << polls::cleanLine(option, polls::kMaxOptionChars);
            poll.multiple  = c.value(QString::fromLatin1("multiple")).toBool();
            poll.channelId = ts3::ownChannel(sch);
            const QString    text = messageFor(poll, sch);
            const anyID      clid = ts3::clientIdByNickname(sch, to);
            const QByteArray utf8 = text.toUtf8();
            const unsigned   e    = clid && !text.isEmpty() ? ts3::funcs.requestSendPrivateTextMsg(sch, utf8.constData(), clid, nullptr) : 1u;
            return QString::fromLatin1("poll.post: privately to %1 (clid %2): error %3; raw \"%4\"").arg(to).arg(clid).arg(e).arg(text.left(600));
        }
        const QString problem = post(sch, c.value(QString::fromLatin1("question")).toString(), options, c.value(QString::fromLatin1("multiple")).toBool());
        return QString::fromLatin1("poll.post: ") + (problem.isEmpty() ? QString::fromLatin1("sent") : problem);
    }
    if (cmd == QLatin1String("poll.attach")) { // opens the paperclip's menu in the visible chat input
        for (const QPointer<QWidget>& input : qAsConst(m_chat->m_inputs)) {
            AttachButton* button = input && input->isVisible() ? AttachButton::of(input) : nullptr;
            if (!button)
                continue;
            button->openMenu();
            QStringList ids;
            if (AttachPopup* popup = button->popup()) {
                for (int i = 0; i < popup->count(); ++i)
                    ids << popup->entryId(i) + (popup->item(i)->isEnabled() ? QString() : QString::fromLatin1("(off)"));
            }
            return QString::fromLatin1("poll.attach: %1 at %2,%3").arg(ids.join(QLatin1Char(','))).arg(button->x()).arg(button->width());
        }
        return QString::fromLatin1("poll.attach: no paperclip in a visible input");
    }
    if (cmd == QLatin1String("poll.creator")) {
        openCreator(nullptr);
        return QString::fromLatin1("poll.creator: %1").arg(m_dialog ? QString::fromLatin1("open") : QString::fromLatin1("not opened"));
    }
    View* view = browser ? viewOf(browser) : nullptr;
    // The cards of the visible chat (oldest first): what each shows (stateOf) and the hub's count.
    const auto cardsByPosition = [this](View& v) {
        ensurePositions(v);
        QVector<const Card*> list;
        for (const Card& k : qAsConst(v.cards))
            list << &k;
        std::sort(list.begin(), list.end(), [&v](const Card* x, const Card* y) { return v.positions.value(x->object, -1) < v.positions.value(y->object, -1); });
        return list;
    };
    if (cmd == QLatin1String("poll.log")) {
        QString out = QString::fromLatin1("poll.log: %1 card(s)").arg(view ? view->cards.size() : 0);
        if (!view || !browser)
            return out;
        for (const Card* k : cardsByPosition(*view)) {
            const pollart::Card st = stateOf(browser, *k);
            const PollHub::View hv = m_hub->view(k->poll);
            QStringList         counts;
            for (int i = 0; i < k->poll.options.size(); ++i)
                counts << QString::fromLatin1("%1:%2/%3%").arg(i + 1).arg(hv.tally.counts.value(i)).arg(hv.percents.value(i));
            out += QString::fromLatin1(" || %1 at %2 %3 \"%4\" [%5] voters %6 own %7 results %8 canVote %9 note \"%10\" state %11 locked %12 watched %13 channel %14 selected %15")
                       .arg(k->poll.key.left(12))
                       .arg(view->positions.value(k->object, -1))
                       .arg(k->poll.multiple ? QString::fromLatin1("multiple") : QString::fromLatin1("single"))
                       .arg(k->poll.question.left(60), counts.join(QLatin1Char(' ')))
                       .arg(hv.tally.voters)
                       .arg(hv.own)
                       .arg(st.results ? 1 : 0)
                       .arg(st.canVote ? 1 : 0)
                       .arg(st.note)
                       .arg(static_cast<int>(hv.state))
                       .arg(hv.locked ? 1 : 0)
                       .arg(m_hub->watching(k->poll.key, k->poll.channelId) ? 1 : 0)
                       .arg(k->poll.channelId)
                       .arg(k->selected);
            out += QString::fromLatin1(" | shown: ") + pollart::describe(st).replace(QLatin1Char('\n'), QString::fromLatin1(" / "));
        }
        return out;
    }
    // Live pass A: the mouse on a card (the real path through the chat's event filter): an option's row
    // (option: 1-based), or zone: action (Vote / Retract vote / Retry), badge, question. hover: only the move;
    // tip: its tool tip; otherwise a left click.
    if (cmd == QLatin1String("poll.click")) {
        if (!view || view->cards.isEmpty() || !browser)
            return cmd + QString::fromLatin1(": no poll card in the visible chat");
        const QString wanted = c.value(QString::fromLatin1("key")).toString();
        const Card*   card   = nullptr;
        for (const Card* k : cardsByPosition(*view)) {
            if (wanted.isEmpty() || k->poll.key.startsWith(wanted))
                card = k; // the newest match
        }
        if (!card)
            return cmd + QString::fromLatin1(": no such poll");
        const int position = view->positions.value(card->object, -1);
        if (position < 0)
            return cmd + QString::fromLatin1(": the card has no position");
        const QTextBlock block = view->document->findBlock(position);
        const QRectF     br    = view->document->documentLayout()->blockBoundingRect(block);
        QScrollBar*      bar   = browser->verticalScrollBar();
        if (br.top() < bar->value() || br.bottom() > bar->value() + browser->viewport()->height())
            bar->setValue(qBound(bar->minimum(), qRound(br.top()) - 40, bar->maximum()));
        QTextCursor cursor(view->document);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat format = cursor.charFormat().toImageFormat();
        const QRectF           rect   = cardRect(browser, position, QSizeF(format.width(), format.height()));
        const pollart::Card    st     = stateOf(browser, *card);
        const pollart::Layout  l      = pollart::layout(st, styleOf(browser));
        const QString          zone   = c.value(QString::fromLatin1("zone")).toString(QString::fromLatin1("action"));
        const bool             option = c.contains(QString::fromLatin1("option"));
        QRectF                 target;
        if (option)
            target = l.rows.value(c.value(QString::fromLatin1("option")).toInt(1) - 1);
        else if (zone == QLatin1String("badge"))
            target = l.badge;
        else if (zone == QLatin1String("question"))
            target = l.question;
        else
            target = l.action;
        if (target.isEmpty())
            return cmd + QString::fromLatin1(": nothing to point at (%1)").arg(option ? QString::fromLatin1("option") : zone);
        QWidget*     vp    = browser->viewport();
        const QPoint at    = (rect.topLeft() + target.center()).toPoint();
        const auto   mouse = [vp](QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(pos), QPointF(vp->mapTo(vp->window(), pos)), QPointF(vp->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(vp, &e);
        };
        mouse(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
        QString out = QString::fromLatin1("%1: %2 at %3,%4 (card %5,%6 %7x%8): hover zone %9 index %10, cursor %11")
                          .arg(cmd, option ? QString::fromLatin1("option %1").arg(c.value(QString::fromLatin1("option")).toInt(1)) : zone)
                          .arg(at.x())
                          .arg(at.y())
                          .arg(qRound(rect.x()))
                          .arg(qRound(rect.y()))
                          .arg(qRound(rect.width()))
                          .arg(qRound(rect.height()))
                          .arg(static_cast<int>(m_hover.zone))
                          .arg(m_hover.index)
                          .arg(static_cast<int>(vp->cursor().shape()));
        if (c.value(QString::fromLatin1("tip")).toBool()) {
            QHelpEvent tip(QEvent::ToolTip, at, vp->mapToGlobal(at));
            QApplication::sendEvent(vp, &tip);
            out += QString::fromLatin1("; tool tip \"%1\"").arg(QToolTip::text());
        } else if (!c.value(QString::fromLatin1("hover")).toBool()) {
            mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
            const PollHub::View hv = m_hub->view(card->poll);
            out += QString::fromLatin1("; clicked: own %1 voters %2 state %3 selected %4").arg(hv.own).arg(hv.tally.voters).arg(static_cast<int>(hv.state)).arg(card->selected);
        }
        return out;
    }
    if (cmd == QLatin1String("poll.vote") || cmd == QLatin1String("poll.retract") || cmd == QLatin1String("poll.retry")) {
        // The newest card of the visible chat (or the one whose key starts with "key").
        if (!view || view->cards.isEmpty())
            return cmd + QString::fromLatin1(": no poll card in the visible chat");
        ensurePositions(*view);
        const QString wanted = c.value(QString::fromLatin1("key")).toString();
        const Card*   card   = nullptr;
        int           best   = -1;
        for (const Card& k : qAsConst(view->cards)) {
            const int position = view->positions.value(k.object, -1);
            if ((wanted.isEmpty() && position > best) || (!wanted.isEmpty() && k.poll.key.startsWith(wanted))) {
                best = position;
                card = &k;
            }
        }
        if (!card)
            return cmd + QString::fromLatin1(": no such poll");
        if (cmd == QLatin1String("poll.retry")) {
            m_hub->retry(sch, card->poll);
            return cmd + QString::fromLatin1(": sent again");
        }
        quint16 mask = static_cast<quint16>(c.value(QString::fromLatin1("mask")).toInt(0));
        if (mask == 0 && c.contains(QString::fromLatin1("option")))
            mask = static_cast<quint16>(1u << qBound(0, c.value(QString::fromLatin1("option")).toInt(1) - 1, polls::kMaxOptions - 1));
        if (cmd == QLatin1String("poll.retract"))
            mask = 0;
        const PollHub::VoteError error = m_hub->vote(sch, card->poll, mask);
        refreshSizes(browser);
        return cmd + QString::fromLatin1(": ") + (error == PollHub::VoteError::None ? QString::fromLatin1("ok") : PollHub::errorText(error));
    }
    return QString::fromLatin1("unknown poll command ") + cmd;
}
#endif
