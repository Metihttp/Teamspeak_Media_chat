#include "chattextspoilers.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>

#include <climits>

#include "chatscroll.h"
#include "i18n.h"
#include "polldoc.h"
#include "replydoc.h"
#include "settings.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "textspoiler.h"
#include "textspoilerdoc.h"

#ifdef TSMEDIA_TESTHOOKS
// Test builds: the paint meter on a chat's viewport (selftestformat.cpp).
QObject* selfTestFormatMeter(ChatTextSpoilers* spoilers, QTextBrowser* browser);
#endif

namespace {

constexpr int kMaxRevealedKeys = 4000;

// A raw pair of a message block, with its key and whether it goes straight to the revealed state.
struct BlockPairs {
    int                           block = -1;
    QVector<textspoilerdoc::Pair> pairs;
    QVector<QString>              keys;
    QVector<bool>                 revealed;
};

// The raw pairs a pass hides in block: a chat message's text (never its header or a reply's quote line), not a poll.
QVector<textspoilerdoc::Pair> hideablePairs(const QTextBlock& block)
{
    if (!textspoiler::mayHold(block.text()))
        return {};
    const replydoc::Message m = replydoc::parseBlock(block);
    if (m.block < 0)
        return {}; // not a chat message (a status line, one of TS Media's own lines)
    if (polldoc::parseBlock(block).block >= 0)
        return {}; // a poll: its card takes the text
    return textspoilerdoc::rawPairs(block, m.textStart);
}

} // namespace

ChatTextSpoilers::ChatTextSpoilers(Host host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
{
    m_handler = new textspoilerdoc::Handler(this);
    m_early   = new QTimer(this);
    m_early->setSingleShot(true);
    m_early->setInterval(0);
    connect(m_early, &QTimer::timeout, this, &ChatTextSpoilers::runEarlyPass);
    m_thaw = new QTimer(this);
    m_thaw->setSingleShot(true);
    m_thaw->setInterval(300);
    connect(m_thaw, &QTimer::timeout, this, &ChatTextSpoilers::thawAll);
    m_lastReveal = Settings::instance().revealSpoilers;
}

ChatTextSpoilers::~ChatTextSpoilers()
{
    m_shutdown = true;
    m_early->stop();
    m_thaw->stop();
    thawAll(); // never leave a chat that doesn't paint
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->meter && it->browser)
            it->browser->viewport()->removeEventFilter(it->meter);
    }
    restoreAll();
    for (const QPointer<QTextDocument>& doc : qAsConst(m_documents)) {
        if (doc && doc->documentLayout())
            doc->documentLayout()->unregisterHandler(textspoilerdoc::kObjectType, m_handler);
    }
    m_documents.clear();
    if (m_cursorSet && m_hoverBrowser)
        m_hoverBrowser->viewport()->setCursor(m_savedCursor); // the chat's own cursor back
    m_cursorSet = false;
    delete m_handler; // also unregisters it from any layout we lost track of (registerHandler ties it to destroyed)
    m_handler = nullptr;
}

ChatTextSpoilers::Colors ChatTextSpoilers::paletteColors(QTextBrowser* browser)
{
    Colors c;
    if (!browser)
        return Colors{QColor(Qt::white), QColor(Qt::black)};
    const QPalette pal  = browser->palette();
    const QBrush   base = pal.brush(QPalette::Base);
    c.text              = pal.color(QPalette::Text);
    if (base.style() == Qt::SolidPattern && base.color().isValid())
        c.base = base.color();
    else
        c.base = c.text.lightness() > 170 ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white);
    return c;
}

ChatTextSpoilers::View* ChatTextSpoilers::viewFor(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

ChatTextSpoilers::View* ChatTextSpoilers::viewForViewport(QObject* viewport)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser && it->browser->viewport() == viewport)
            return &it.value();
    }
    return nullptr;
}

void ChatTextSpoilers::ensureHandler(QTextDocument* doc)
{
    if (!doc || !doc->documentLayout())
        return;
    if (doc->documentLayout()->handlerForObject(textspoilerdoc::kObjectType) != m_handler)
        doc->documentLayout()->registerHandler(textspoilerdoc::kObjectType, m_handler);
    m_documents.removeAll(QPointer<QTextDocument>());
    if (!m_documents.contains(QPointer<QTextDocument>(doc)))
        m_documents.append(doc);
}

void ChatTextSpoilers::attach(QTextBrowser* browser)
{
    if (!browser || m_views.contains(browser) || m_shutdown)
        return;
    View v;
    v.browser  = browser;
    v.document = browser->document();
    m_views.insert(browser, v);
    View& view = m_views[browser];
    ensureHandler(view.document);
    watch(view);
    updateLook(view);
    connect(browser, &QObject::destroyed, this, [this, browser] { m_views.remove(browser); });
#ifdef TSMEDIA_TESTHOOKS
    view.meter = selfTestFormatMeter(this, browser);
    if (view.meter)
        browser->viewport()->installEventFilter(view.meter);
#endif
    // What the chat holds already: before it is painted again.
    view.earlyFrom = 0;
    m_early->start();
}

void ChatTextSpoilers::documentSwapped(QTextBrowser* browser)
{
    View* view = viewFor(browser);
    if (!view || view->document == browser->document())
        return;
    // The old document keeps its objects and its handler (TeamSpeak may show it again); it is given back on unload.
    view->document  = browser->document();
    view->dirtyFrom = 0;
    view->earlyFrom = 0;
    ensureHandler(view->document);
    watch(*view);
    updateLook(*view);
    m_early->start();
}

void ChatTextSpoilers::watch(View& view)
{
    QTextDocument* doc = view.document;
    if (!doc)
        return;
    disconnect(doc, &QTextDocument::contentsChange, this, nullptr);
    const QPointer<QTextBrowser> browser = view.browser;
    connect(doc, &QTextDocument::contentsChange, this, [this, doc, browser](int position, int removed, int added) {
        Q_UNUSED(removed)
        if (m_editing || m_shutdown || !browser)
            return;
        View* v = viewFor(browser.data());
        if (!v || v->document != doc)
            return;
        v->dirtyFrom = qMin(v->dirtyFrom, position);
        if (added <= 0 || (m_host.othersMutating && m_host.othersMutating()))
            return; // another module's edit: the scan's pass looks at it
        // Inserted text with an opening marker: hidden before TeamSpeak paints it (a 0 ms timer runs ahead of Qt's
        // low-priority repaint). Never an edit inside this signal.
        const int last = qMax(0, doc->characterCount() - 1);
        for (QTextBlock b = doc->findBlock(qBound(0, position, last)); b.isValid() && b.position() <= position + added; b = b.next()) {
            if (textspoiler::mayHold(b.text())) {
                v->earlyFrom = v->earlyFrom < 0 ? b.position() : qMin(v->earlyFrom, b.position());
                freeze(*v); // not an edit: nothing paints the raw pair before the pass
                m_early->start();
                break;
            }
        }
    });
}

void ChatTextSpoilers::updateLook(View& view)
{
    if (!view.browser || !view.document)
        return;
    const Colors          colors = m_host.colors ? m_host.colors(view.browser.data()) : paletteColors(view.browser.data());
    textspoilerdoc::Look  look   = m_handler->look(view.document.data());
    look.base                    = colors.base;
    look.text                    = colors.text;
    m_handler->setLook(view.document.data(), look);
}

QString ChatTextSpoilers::keyFor(QTextDocument* doc, int block, int index) const
{
    const QTextBlock b    = doc->findBlockByNumber(block);
    QString          hash = textreactdoc::identify(b).hash;
    if (hash.isEmpty()) {
        const replydoc::Message m = replydoc::parseBlock(b);
        hash                      = textreact::contentHash(m.uid, m.text);
    }
    return hash + QLatin1Char(':') + QString::number(index);
}

bool ChatTextSpoilers::earlyPending() const
{
    return m_early->isActive();
}

void ChatTextSpoilers::freeze(View& view)
{
    QWidget* viewport = view.browser ? view.browser->viewport() : nullptr;
    if (view.frozen || !viewport || !viewport->updatesEnabled())
        return; // already off (ours, or someone else's: theirs to turn on)
    viewport->setUpdatesEnabled(false);
    view.frozen = true;
    m_thaw->start();
}

void ChatTextSpoilers::thaw(View& view)
{
    if (!view.frozen)
        return;
    view.frozen = false;
    if (view.browser)
        view.browser->viewport()->setUpdatesEnabled(true); // repaints what changed meanwhile
}

void ChatTextSpoilers::thawAll()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        thaw(it.value());
}

void ChatTextSpoilers::runEarlyPass()
{
    if (m_shutdown)
        return;
    const bool busy = m_host.othersMutating && m_host.othersMutating();
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        const int from = it->earlyFrom;
        it->earlyFrom  = -1;
        if (from >= 0 && !busy) // someone edits the chat right now: the scan's pass does it
            pass(it.value(), from);
        if (!busy) // else the scan's pass turns its updates on (m_thaw at the latest)
            thaw(it.value());
    }
}

void ChatTextSpoilers::afterScan(QTextBrowser* browser)
{
    View* view = viewFor(browser);
    if (!view || m_shutdown)
        return;
    if (view->document != browser->document())
        documentSwapped(browser);
    const int from  = view->dirtyFrom;
    view->dirtyFrom = INT_MAX;
    view->earlyFrom = -1;
    updateLook(*view);
    if (from != INT_MAX)
        pass(*view, from);
    thaw(*view);
}

void ChatTextSpoilers::pass(View& view, int from)
{
    QTextBrowser*  browser = view.browser.data();
    QTextDocument* doc     = view.document.data();
    if (!browser || !doc || browser->document() != doc || m_shutdown)
        return;
    const bool revealAll = Settings::instance().revealSpoilers;
    // The raw pairs of message blocks (the newest kMaxBlocks), read before any edit.
    QVector<BlockPairs> todo;
    const int           firstAllowed = qMax(0, doc->blockCount() - replydoc::kMaxBlocks);
    QTextBlock          b            = doc->findBlock(qBound(0, from, qMax(0, doc->characterCount() - 1)));
    if (b.isValid() && b.blockNumber() < firstAllowed)
        b = doc->findBlockByNumber(firstAllowed);
    for (; b.isValid(); b = b.next()) {
        const QVector<textspoilerdoc::Pair> pairs = hideablePairs(b);
        if (pairs.isEmpty())
            continue;
        // Already hidden pairs of the message come first in its numbering (a later pass finds only the new ones).
        const int  before = textspoilerdoc::spoilersIn(doc, b.blockNumber()).size();
        BlockPairs bp;
        bp.block = b.blockNumber();
        bp.pairs = pairs;
        for (int i = 0; i < pairs.size(); ++i) {
            const QString key = keyFor(doc, bp.block, before + i);
            bp.keys.append(key);
            bp.revealed.append(revealAll || m_revealed.contains(key));
        }
        todo.append(bp);
    }
    if (todo.isEmpty())
        return;
    const chatscroll::TextPlace place = chatscroll::captureText(browser);
    const bool                  was   = m_host.setMutating ? m_host.setMutating(true) : false;
    m_editing                         = true;
    int& serial                       = m_serials[doc];
    {
        QTextCursor batch(doc);
        batch.beginEditBlock();
        for (int i = todo.size() - 1; i >= 0; --i) // back to front: earlier blocks keep their positions
            textspoilerdoc::hide(doc, todo.at(i).pairs, todo.at(i).keys, todo.at(i).revealed, &serial);
        batch.endEditBlock(); // TeamSpeak's view hears of it while we are still "mutating"
    }
    m_editing = false;
    if (m_host.setMutating)
        m_host.setMutating(was);
    chatscroll::restoreText(browser, place);
    if (m_host.hid) {
        for (const BlockPairs& bp : qAsConst(todo))
            m_host.hid(browser, bp.block);
    }
    if (m_host.edited)
        m_host.edited(browser);
}

void ChatTextSpoilers::settingsChanged()
{
    const bool reveal = Settings::instance().revealSpoilers;
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        it->dirtyFrom = 0; // a full pass at the next scan
    if (reveal == m_lastReveal || m_shutdown)
        return;
    m_lastReveal = reveal;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        QTextBrowser*  browser = it->browser.data();
        QTextDocument* doc     = it->document.data();
        if (!browser || !doc || browser->document() != doc)
            continue;
        const QVector<textspoilerdoc::Spoiler> all = textspoilerdoc::spoilersIn(doc);
        if (all.isEmpty())
            continue;
        const chatscroll::TextPlace place = chatscroll::captureText(browser);
        m_editing                         = true;
        if (reveal) {
            // Everything shown (not remembered as clicked: turning it off again hides these).
            for (int i = all.size() - 1; i >= 0; --i)
                textspoilerdoc::reveal(doc, all.at(i));
            m_editing = false;
        } else {
            // What the setting revealed goes back to hidden, unless a link preview came into it meanwhile.
            int  first   = INT_MAX;
            const bool was = m_host.setMutating ? m_host.setMutating(true) : false;
            for (int i = all.size() - 1; i >= 0; --i) {
                const textspoilerdoc::Spoiler& s = all.at(i);
                if (!s.revealed || m_revealed.contains(s.key) || textspoilerdoc::holdsOthers(doc, s))
                    continue;
                if (textspoilerdoc::unhide(doc, s))
                    first = qMin(first, doc->findBlockByNumber(s.block).position());
            }
            if (m_host.setMutating)
                m_host.setMutating(was);
            m_editing = false;
            if (first != INT_MAX)
                pass(it.value(), first);
        }
        chatscroll::restoreText(browser, place);
        if (m_host.edited)
            m_host.edited(browser);
        browser->viewport()->update();
    }
}

void ChatTextSpoilers::restoreAll()
{
    m_editing = true;
    for (const QPointer<QTextDocument>& doc : qAsConst(m_documents)) {
        if (doc)
            textspoilerdoc::restoreAll(doc.data());
    }
    m_editing = false;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->document)
            m_handler->forget(it->document.data());
    }
}

void ChatTextSpoilers::rememberRevealed(const QString& key)
{
    if (key.isEmpty() || m_revealed.contains(key))
        return;
    m_revealed.insert(key);
    m_revealedOrder.append(key);
    while (m_revealedOrder.size() > kMaxRevealedKeys)
        m_revealed.remove(m_revealedOrder.takeFirst());
}

// ---- revealing ---------------------------------------------------------------------------------------------

void ChatTextSpoilers::revealSpoiler(QTextBrowser* browser, int id, bool clicked)
{
    View* view = viewFor(browser);
    if (!view || !view->document || browser->document() != view->document || id < 0)
        return;
    QTextDocument* doc = view->document.data();
    for (const textspoilerdoc::Spoiler& s : textspoilerdoc::spoilersIn(doc)) {
        if (s.id != id || s.revealed)
            continue;
        const chatscroll::TextPlace place = chatscroll::captureText(browser);
        // Not "mutating": ChatEmoji draws its emoji, the next scan gives a link its preview (the normal rules).
        m_editing = true;
        textspoilerdoc::reveal(doc, s);
        m_editing = false;
        chatscroll::restoreText(browser, place);
        if (clicked)
            rememberRevealed(s.key);
        m_revealMs = QDateTime::currentMSecsSinceEpoch();
        if (m_host.edited)
            m_host.edited(browser);
        break;
    }
    if (m_hoverId == id)
        setHover(browser, -1);
    browser->viewport()->update();
}

void ChatTextSpoilers::revealBlock(QTextBrowser* browser, int block)
{
    View* view = viewFor(browser);
    if (!view || !view->document || browser->document() != view->document)
        return;
    const QVector<textspoilerdoc::Spoiler> all = textspoilerdoc::spoilersIn(view->document.data(), block);
    for (int i = all.size() - 1; i >= 0; --i) { // the last first: the earlier ones keep their positions
        if (!all.at(i).revealed)
            revealSpoiler(browser, all.at(i).id, true);
    }
}

QAction* ChatTextSpoilers::revealAction(QMenu* menu, QTextBrowser* browser, int block)
{
    View* view = viewFor(browser);
    if (!menu || !view || !view->document || browser->document() != view->document || block < 0)
        return nullptr;
    int hidden = 0;
    for (const textspoilerdoc::Spoiler& s : textspoilerdoc::spoilersIn(view->document.data(), block))
        hidden += s.revealed ? 0 : 1;
    if (hidden == 0)
        return nullptr;
    auto* action = new QAction(hidden == 1 ? i18n::t("Re&veal spoiler") : i18n::t("Re&veal spoilers"), menu);
    const QPointer<QTextBrowser> guard(browser);
    connect(action, &QAction::triggered, this, [this, guard, block] {
        if (guard)
            revealBlock(guard.data(), block);
    });
    return action;
}

int ChatTextSpoilers::barAt(QTextBrowser* browser, const QPoint& viewportPos) const
{
    if (!browser)
        return -1;
    QTextDocument* doc = browser->document();
    const QPointF  docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    for (const int p : {hit, hit - 1}) {
        const int id = textspoilerdoc::wordIdAt(doc, p);
        if (id < 0)
            continue;
        const QTextBlock block  = doc->findBlock(p);
        const QTextLayout* lines = block.layout();
        if (!lines)
            continue;
        const QTextLine line = lines->lineForTextPosition(p - block.position());
        if (!line.isValid())
            continue;
        const QRectF br = doc->documentLayout()->blockBoundingRect(block);
        const qreal  x1 = line.cursorToX(p - block.position());
        const qreal  x2 = line.cursorToX(p + 1 - block.position());
        const QRectF r(br.left() + qMin(x1, x2), br.top() + line.y(), qAbs(x2 - x1), line.height());
        if (r.contains(docPos))
            return id;
    }
    return -1;
}

int ChatTextSpoilers::revealAt(QTextBrowser* browser, const QPoint& viewportPos)
{
    const int id = barAt(browser, viewportPos);
    if (id >= 0)
        revealSpoiler(browser, id, true);
    return id;
}

void ChatTextSpoilers::setHover(QTextBrowser* browser, int id)
{
    if (m_hoverBrowser != browser && m_hoverBrowser && m_hoverId >= 0) {
        textspoilerdoc::Look old = m_handler->look(m_hoverBrowser->document());
        old.hovered              = -1;
        m_handler->setLook(m_hoverBrowser->document(), old);
        m_hoverBrowser->viewport()->update();
    }
    if (browser) {
        textspoilerdoc::Look look = m_handler->look(browser->document());
        if (look.hovered != id) {
            look.hovered = id;
            m_handler->setLook(browser->document(), look);
            browser->viewport()->update();
        }
        QWidget* vp = browser->viewport();
        if (id >= 0) {
            if (!m_cursorSet) {
                m_savedCursor = vp->cursor().shape() == Qt::PointingHandCursor ? Qt::ArrowCursor : vp->cursor().shape();
                m_cursorSet   = true;
            }
            if (vp->cursor().shape() != Qt::PointingHandCursor)
                vp->setCursor(Qt::PointingHandCursor);
        } else if (m_cursorSet) {
            vp->setCursor(m_savedCursor);
            m_cursorSet = false;
        }
    }
    m_hoverBrowser = browser;
    m_hoverId      = id;
}

void ChatTextSpoilers::setPressed(QTextBrowser* browser, int id)
{
    if (browser) {
        textspoilerdoc::Look look = m_handler->look(browser->document());
        if (look.pressed != id) {
            look.pressed = id;
            m_handler->setLook(browser->document(), look);
            browser->viewport()->update();
        }
    }
    m_pressBrowser = id >= 0 ? browser : nullptr;
    m_pressId      = id;
}

bool ChatTextSpoilers::filterEvent(QObject* watched, QEvent* event)
{
    if (m_shutdown)
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick
        && type != QEvent::ToolTip && type != QEvent::Leave)
        return false;
    View* view = viewForViewport(watched);
    if (!view || !view->browser)
        return false;
    QTextBrowser* browser = view->browser.data();
    switch (type) {
    case QEvent::MouseMove: {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->buttons() == Qt::NoButton || m_pressId >= 0)
            setHover(browser, barAt(browser, me->pos()));
        return m_pressId >= 0 && m_pressBrowser == browser; // a press on a bar starts no selection
    }
    case QEvent::MouseButtonPress: {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        const int id = barAt(browser, me->pos());
        if (id < 0)
            return false;
        setPressed(browser, id);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || m_pressId < 0 || m_pressBrowser != browser)
            return false;
        const int pressed = m_pressId;
        setPressed(browser, -1);
        if (barAt(browser, me->pos()) == pressed)
            revealSpoiler(browser, pressed, true);
        return true;
    }
    case QEvent::MouseButtonDblClick: {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        if (barAt(browser, me->pos()) >= 0)
            return true;
        return QDateTime::currentMSecsSinceEpoch() - m_revealMs < QApplication::doubleClickInterval();
    }
    case QEvent::ToolTip: {
        const auto* he = static_cast<QHelpEvent*>(event);
        if (barAt(browser, he->pos()) < 0)
            return false;
        QToolTip::showText(he->globalPos(), m_host.toolTip ? m_host.toolTip() : i18n::t("Spoiler · Click to reveal"), browser->viewport());
        return true;
    }
    case QEvent::Leave:
        if (m_hoverBrowser == browser)
            setHover(browser, -1);
        return false;
    default:
        break;
    }
    return false;
}

// ---- counts and the driver ---------------------------------------------------------------------------------

int ChatTextSpoilers::hiddenCount() const
{
    int n = 0;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (!it->document)
            continue;
        for (const textspoilerdoc::Spoiler& s : textspoilerdoc::spoilersIn(it->document.data()))
            n += s.revealed ? 0 : 1;
    }
    return n;
}

int ChatTextSpoilers::revealedCount() const
{
    int n = 0;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (!it->document)
            continue;
        for (const textspoilerdoc::Spoiler& s : textspoilerdoc::spoilersIn(it->document.data()))
            n += s.revealed ? 1 : 0;
    }
    return n;
}

QStringList ChatTextSpoilers::diagnosticLines() const
{
    return {i18n::t("Text spoilers: hidden %1, revealed %2").arg(hiddenCount()).arg(revealedCount())};
}

QVector<QRect> ChatTextSpoilers::barRects(QTextBrowser* browser, int id) const
{
    QVector<QRect> out;
    if (!browser)
        return out;
    QTextDocument* doc = browser->document();
    const QPointF  scroll(browser->horizontalScrollBar()->value(), browser->verticalScrollBar()->value());
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (!textspoiler::mayHold(b.text()) && !b.text().contains(QChar::ObjectReplacementCharacter))
            continue;
        const QTextLayout* lines = b.layout();
        if (!lines || !b.isVisible())
            continue;
        const QRectF br = doc->documentLayout()->blockBoundingRect(b);
        for (int p = b.position(); p < b.position() + b.length() - 1; ++p) {
            const int at = textspoilerdoc::wordIdAt(doc, p);
            if (at < 0 || (id >= 0 && at != id))
                continue;
            const QTextLine line = lines->lineForTextPosition(p - b.position());
            if (!line.isValid())
                continue;
            const qreal x1 = line.cursorToX(p - b.position());
            const qreal x2 = line.cursorToX(p + 1 - b.position());
            out.append(QRectF(br.left() + qMin(x1, x2) - scroll.x(), br.top() + line.y() - scroll.y(), qAbs(x2 - x1), line.height()).toAlignedRect());
        }
    }
    return out;
}

int ChatTextSpoilers::rawPairsShown(QTextBrowser* browser) const
{
    if (!browser)
        return 0;
    QTextDocument* doc = browser->document();
    const qreal    top = browser->verticalScrollBar()->value();
    const qreal    bottom = top + browser->viewport()->height();
    const int      hit = doc->documentLayout()->hitTest(QPointF(doc->documentMargin() + 1, top + 1), Qt::FuzzyHit);
    int            n   = 0;
    for (QTextBlock b = doc->findBlock(qMax(0, hit)); b.isValid(); b = b.next()) {
        const QRectF r = doc->documentLayout()->blockBoundingRect(b);
        if (r.top() > bottom)
            break;
        if (b.isVisible() && r.bottom() >= top)
            n += hideablePairs(b).size();
    }
    return n;
}

QStringList ChatTextSpoilers::describe(QTextBrowser* browser) const
{
    QStringList out;
    if (!browser)
        return out;
    for (const textspoilerdoc::Spoiler& s : textspoilerdoc::spoilersIn(browser->document())) {
        QStringList rects;
        for (const QRect& r : barRects(browser, s.id))
            rects << QString::fromLatin1("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        out << QString::fromLatin1("block %1 id %2 %3, %4 bar(s): %5")
                   .arg(s.block)
                   .arg(s.id)
                   .arg(s.revealed ? QString::fromLatin1("revealed") : QString::fromLatin1("hidden"))
                   .arg(rects.size())
                   .arg(rects.join(QLatin1String("; ")));
    }
    return out;
}
