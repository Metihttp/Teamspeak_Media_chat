#include "chattextreactions.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include "chatemoji.h"
#include "chatintegration.h"
#include "chatlayout.h"
#include "chatreplies.h"
#include "emojidata.h"
#include "emojipicker.h"
#include "emojiprefs.h"
#include "i18n.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "peerhub.h"
#include "peers.h"
#include "pluginlink.h"
#include "reactionpicker.h"
#include "replydoc.h"
#include "settings.h"
#include "textreactdoc.h"
#include "textreacthub.h"
#include "ts3api.h"

namespace {

constexpr int kKindRecheckMs = 2000;
constexpr int kNamesInTip    = 10;
constexpr int kMaxDrawn      = 48;  // row pictures kept drawn per chat (the others get the blank)
constexpr int kShownMargin   = 400; // px above and below the viewport drawn ahead
constexpr int kRelayoutMs    = 120; // after the chat's last resize
constexpr int kIndexBudgetMs  = 6;   // reading messages, per turn
constexpr int kIndexStepMs    = 16;  // ... and the next turn after
constexpr int kReadSoonMs     = 200; // what the layout or the HD emoji restyled is read again after (bursts merge)

bool reactive(rx::Zone zone)
{
    return zone == rx::Zone::Pill || zone == rx::Zone::AddPill || zone == rx::Zone::Row;
}

bool actionable(rx::Zone zone)
{
    return zone == rx::Zone::Pill || zone == rx::Zone::AddPill;
}

// "you, Sara and Reza"; "Ali, Sara, ... and 4 others" (as under media).
QString nameList(QStringList names, int total)
{
    for (QString& name : names)
        name = peers::presenceName(name);
    if (names.size() > kNamesInTip)
        names = names.mid(0, kNamesInTip);
    const int rest = qMax(0, total - names.size());
    if (rest > 0)
        return i18n::t("%1 and %2 others").arg(names.join(QString::fromLatin1(", "))).arg(rest);
    if (names.size() <= 1)
        return names.value(0);
    const QString last = names.takeLast();
    return i18n::t("%1 and %2").arg(names.join(QString::fromLatin1(", ")), last);
}

// Rich text with everything escaped: names come from other people.
QString tipHtml(const QStringList& lines)
{
    QString html;
    for (const QString& line : lines) {
        if (!html.isEmpty())
            html += QString::fromLatin1("<br>");
        html += line.toHtmlEscaped();
    }
    return QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(html);
}

QIcon addIcon(qreal dpr)
{
    QPixmap pixmap(QSize(16, 16) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    rx::drawAddReactionGlyph(p, QRectF(0, 0, 16, 16), QColor(0x7d, 0x82, 0x8a));
    p.end();
    return QIcon(pixmap);
}

// The reader's place across our edits: the block at the top of the chat and how far into it (a row grows
// at the end of its block, so what is above it stays put), or the bottom.
struct Place {
    bool  bottom = false;
    int   block  = -1;
    qreal offset = 0;
};

Place capturePlace(QTextBrowser* browser)
{
    Place      p;
    QScrollBar* bar = browser->verticalScrollBar();
    p.bottom        = bar->value() >= bar->maximum() - 4;
    const layoutdoc::Anchor a = layoutdoc::anchorAt(browser->document(), bar->value());
    p.block                   = a.block;
    p.offset                  = a.offset;
    return p;
}

// A chat tab that isn't shown, of the server tab that is: its place among TeamSpeak's chat tabs (the
// browser is a page of the ChatTabWidget, a QTabWidget). -1: not one (another server tab's, or not a tab).
int hiddenTabIndex(QTextBrowser* browser)
{
    QWidget* stack = browser ? browser->parentWidget() : nullptr;
    auto*    tabs  = stack ? qobject_cast<QTabWidget*>(stack->parentWidget()) : nullptr;
    if (!tabs || !tabs->isVisible() || !tabs->inherits("ChatTabWidget"))
        return -1;
    return tabs->indexOf(browser);
}

void restorePlace(QTextBrowser* browser, const Place& p)
{
    QTextDocument*               doc    = browser->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const QSizeF                 size   = layout->documentSize(); // lays out what changed
    QScrollBar*                  bar    = browser->verticalScrollBar();
    const int                    want   = qMax(0, qRound(size.height()) - browser->viewport()->height());
    if (bar->maximum() != want)
        emit layout->documentSizeChanged(size); // QTextEdit takes the new range now (chatscroll.h)
    if (p.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    const QTextBlock b = doc->findBlockByNumber(p.block);
    if (p.block >= 0 && b.isValid() && b.isVisible())
        bar->setValue(qRound(layout->blockBoundingRect(b).top() + p.offset));
}

} // namespace

ChatTextReactions::ChatTextReactions(ChatIntegration* chat)
    : QObject(chat)
    , m_chat(chat)
    , m_hub(TextReactHub::instance())
{
    m_tag   = QString::number(QRandomGenerator::global()->generate(), 16);
    m_blank = QPixmap(1, 1);
    m_blank.fill(Qt::transparent);
    m_shown = Settings::instance().showReactions;
    m_relayout = new QTimer(this); // a child: it goes with us
    m_relayout->setSingleShot(true);
    m_relayout->setInterval(kRelayoutMs);
    connect(m_relayout, &QTimer::timeout, this, [this] {
        const QSet<QTextBrowser*> browsers = m_relayoutViews;
        m_relayoutViews.clear();
        for (QTextBrowser* browser : browsers) {
            const auto it = m_views.find(browser); // a chat that went meanwhile is no longer here
            if (it != m_views.end() && it->browser)
                reconcileAll(it.value());
        }
    });
    m_indexTimer = new QTimer(this); // a child: it goes with us
    m_indexTimer->setSingleShot(true);
    m_indexTimer->setInterval(kIndexStepMs);
    connect(m_indexTimer, &QTimer::timeout, this, [this] {
        const QSet<QTextBrowser*> browsers = m_indexViews;
        m_indexViews.clear();
        for (QTextBrowser* browser : browsers) {
            const auto it = m_views.find(browser); // a chat that went meanwhile is no longer here
            if (it == m_views.end() || !it->browser || it->document != it->browser->document())
                continue;
            classify(it.value(), false); // a chat just shown (see the Paint case of filterEvent)
            if (index(it.value()))
                publish(it.value());
        }
    });
    // The layout and the HD emoji restyle messages without a rescan: what they changed is read again soon
    // (the action bar would otherwise read those messages on every mouse move).
    auto readSoon = [this](QTextBrowser* browser) {
        if (browser && m_views.contains(browser)) {
            m_indexViews.insert(browser);
            if (!m_indexTimer->isActive())
                m_indexTimer->start(kReadSoonMs);
        }
    };
    if (m_chat->m_layout)
        connect(m_chat->m_layout, &ChatLayout::documentEdited, this, readSoon);
    if (m_chat->m_emoji)
        connect(m_chat->m_emoji, &ChatEmoji::documentEdited, this, readSoon);
    if (m_hub) {
        connect(m_hub, &TextReactHub::reactionsChanged, this, &ChatTextReactions::refreshKey);
        connect(m_hub, &TextReactHub::reactionFailed, this, [this](const QString& key, const QString& text) {
            // At the pointer, if the chat with that message is in view; the log has it either way.
            QTextBrowser* shown = m_chat->visibleChatBrowser();
            View*         view  = shown ? viewFor(shown) : nullptr;
            if (view && view->blocksByKey.contains(key))
                m_chat->showFeedback(shown->viewport(), text, true);
        });
    }
}

ChatTextReactions::~ChatTextReactions()
{
    if (m_picker)
        delete m_picker.data();
    if (m_fullPicker)
        delete m_fullPicker.data();
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (m_hub)
            m_hub->index().removeChat(reinterpret_cast<quintptr>(it.key()));
        if (it->document)
            giveBack(it->document.data(), &it.value());
    }
    if (m_hub)
        m_hub->indexChanged();
}

// ---- views ------------------------------------------------------------------------------------------

ChatTextReactions::View* ChatTextReactions::viewFor(QTextBrowser* browser)
{
    if (!browser)
        return nullptr;
    auto it = m_views.find(browser);
    if (it == m_views.end()) {
        View v;
        v.browser  = browser;
        v.document = browser->document();
        it         = m_views.insert(browser, v);
        track(browser);
    }
    return &it.value();
}

ChatTextReactions::View* ChatTextReactions::viewForViewport(QObject* viewport)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser && it->browser->viewport() == viewport)
            return &it.value();
    }
    return nullptr;
}

void ChatTextReactions::track(QTextBrowser* browser)
{
    if (m_tracked.contains(browser))
        return;
    m_tracked.insert(browser);
    connect(browser, &QObject::destroyed, this, [this, browser] {
        m_tracked.remove(browser);
        m_views.remove(browser);
        if (m_hub) {
            m_hub->index().removeChat(reinterpret_cast<quintptr>(browser));
            m_hub->indexChanged();
        }
    });
}

void ChatTextReactions::giveBack(QTextDocument* doc, View* view)
{
    if (!doc)
        return;
    QStringList names;
    {
        layoutformat::MutatingScope mutating; // nobody rescans for it
        QTextCursor                 batch(doc);
        batch.beginEditBlock();
        textreactdoc::removeAll(doc, &names);
        batch.endEditBlock();
    }
    if (view) {
        for (auto it = view->rendered.constBegin(); it != view->rendered.constEnd(); ++it)
            names.append(it.key());
        view->rendered.clear();
    }
    for (const QString& name : qAsConst(names))
        doc->addResource(QTextDocument::ImageResource, QUrl(name), QVariant()); // the picture's memory goes
}

void ChatTextReactions::classify(View& v, bool force)
{
    QTextBrowser* browser = v.browser;
    const qint64  now     = QDateTime::currentMSecsSinceEpoch();
    if (!browser || (!force && v.kind != textreact::ChatKind::Unknown && now - v.kindCheckedMs < kKindRecheckMs))
        return;
    // Only a chat that is shown is the current tab of its tab bar.
    if (!browser->isVisible() || !m_chat->chatTabBarFor(browser)) {
        // Not shown: what it is follows from its place (the channel chat while the server tab is shown is
        // still the channel chat: its messages count for TSYNC). A private chat's partner is found only
        // while it is shown; until then only the partner's own messages take private reactions there.
        if (v.kind != textreact::ChatKind::Unknown)
            return; // what was found while it was shown stays
        const textreact::ChatKind kind = textreact::kindOfTab(hiddenTabIndex(browser));
        const uint64              sch  = ts3::currentConnection();
        if (kind == textreact::ChatKind::Unknown || !ts3::isConnected(sch))
            return;
        v.kindCheckedMs = now;
        v.kind          = kind;
        v.partnerUid.clear();
        v.serverUid = ts3::serverUid(sch);
        publish(v);
        reconcileAll(v); // the server chat gets no rows
        return;
    }
    if (!ts3::isConnected(ts3::currentConnection()))
        return; // resolveTarget can't tell the chats apart then (it says "channel" for every one)
    v.kindCheckedMs = now;
    ChatTarget target;
    textreact::ChatKind kind = textreact::ChatKind::Private; // a private chat whose partner can't be identified
    QString             partner;
    if (m_chat->resolveTarget(browser, &target)) {
        kind    = target.mode == TextMessageTarget_SERVER ? textreact::ChatKind::Server
                  : target.mode == TextMessageTarget_CLIENT ? textreact::ChatKind::Private
                                                            : textreact::ChatKind::Channel;
        partner = target.clientUid;
    }
    const QString server = target.serverUid.isEmpty() ? ts3::serverUid(target.sch) : target.serverUid;
    if (kind == v.kind && partner == v.partnerUid && server == v.serverUid)
        return;
    v.kind       = kind;
    v.partnerUid = partner;
    v.serverUid  = server;
    publish(v);
    reconcileAll(v); // the server chat gets no rows
}

// Two independent 32-bit hashes of the text, and whether the block is shown; never 0 (textreact::carryOver).
quint64 ChatTextReactions::signatureOf(const QTextBlock& block)
{
    const QString text = block.text();
    const quint64 sig  = (static_cast<quint64>(qHash(text, 0x9e3779b9u)) << 32) ^ static_cast<quint64>(qHash(text)) ^ (block.isVisible() ? 2u : 0u);
    return sig ? sig : 1;
}

QString ChatTextReactions::lookKeyOf(const Look& look)
{
    return look.font.key() + QLatin1Char('|') + QString::number(look.maxWidth);
}

// One block as a message: its key when it can get reactions. dates: the document's days, read when first needed
// (with what kept says for messages whose day line the line limit dropped; the blocks moved up by up since).
ChatTextReactions::Entry ChatTextReactions::readEntry(QTextDocument* doc, const QTextBlock& block, QVector<QDate>* dates, bool* datesRead, const QVector<QDate>& kept, int up)
{
    Entry e;
    e.read                          = true;
    e.signature                     = signatureOf(block);
    const textreactdoc::Identity id = textreactdoc::identify(block);
    if (!id.eligible())
        return e;
    const QDate today = QDate::currentDate();
    if (!*datesRead) {
        *dates     = textreact::keepDays(layoutdoc::datesOf(doc, today), kept, up);
        *datesRead = true;
    }
    const int   n   = block.blockNumber();
    const QDate day = textreactdoc::dayOf(n < dates->size() ? dates->at(n) : QDate(), id.seconds, today, QTime::currentTime().msecsSinceStartOfDay() / 1000);
    qint64      t   = 0;
    e.key           = textreactdoc::localKeyOf(id, day, &t);
    e.eligible      = !e.key.isEmpty();
    e.hash          = id.hash;
    e.sender        = id.uid;
    e.timeSec       = t;
    return e;
}

bool ChatTextReactions::index(View& v)
{
    QTextDocument* doc = v.document;
    if (!doc)
        return false;
    const int count = doc->blockCount();
    const int first = qMax(0, count - replydoc::kMaxBlocks);
    // What was read before is found again by its text: once a chat is full, TeamSpeak's line limit drops
    // the first blocks with every new line, and every block number moves (its row moved with it).
    const QVector<Entry> old = v.entries;
    QVector<quint64>     before(old.size(), 0);
    for (int n = 0; n < old.size(); ++n)
        before[n] = old.at(n).read ? old.at(n).signature : 0;
    QVector<quint64> now(count, 0);
    for (QTextBlock b = doc->findBlockByNumber(first); b.isValid(); b = b.next())
        now[b.blockNumber()] = signatureOf(b);
    bool               moved = false;
    const QVector<int> from  = textreact::carryOver(before, now, &moved);
    // The days of the last reading: a message whose day line the line limit dropped keeps its day (else it
    // would read as today's, and its key and reactions would change with the next restyle).
    const int            up   = textreact::movedUp(from);
    const QVector<QDate> kept = up >= 0 ? v.days : QVector<QDate>();
    QVector<Entry>     next(count);
    QVector<QDate>     dates;
    bool               datesRead = false;
    bool               more      = false;
    QVector<int>       touched;
    QElapsedTimer      clock;
    clock.start();
    // Newest first, a few milliseconds at a time: right after the layout styled a long history every block
    // reads differently (its header became a picture), and the rest follows on the timer.
    for (int n = count - 1; n >= first; --n) {
        if (from.at(n) >= 0) {
            next[n] = old.at(from.at(n));
            continue;
        }
        if (more || clock.elapsed() >= kIndexBudgetMs) {
            more = true;
            // Until it is read: what this number was the last time, unless the numbers moved (then it is
            // someone else's).
            if (!moved && n < old.size()) {
                next[n]      = old.at(n);
                next[n].read = false;
            }
            continue;
        }
        next[n] = readEntry(doc, doc->findBlockByNumber(n), &dates, &datesRead, kept, up);
        touched.append(n); // read again: its row is checked
    }
    v.days = datesRead ? dates : textreact::keepDays(QVector<QDate>(count), kept, up);
    if (more && v.browser) {
        m_indexViews.insert(v.browser.data());
        m_indexTimer->start(kIndexStepMs);
    }
    for (int n = 0; n < first && n < old.size(); ++n) {
        if (old.at(n).eligible)
            touched.append(n); // fell out of the newest blocks: its row goes
    }
    bool changed = old.size() != next.size();
    for (int n = 0; !changed && n < next.size(); ++n)
        changed = next.at(n).key != old.at(n).key || next.at(n).eligible != old.at(n).eligible;
    v.entries = next;
    if (changed) {
        v.blocksByKey.clear();
        for (int n = 0; n < v.entries.size(); ++n) {
            if (v.entries.at(n).eligible)
                v.blocksByKey[v.entries.at(n).key].append(n);
        }
    }
    if (!touched.isEmpty())
        reconcile(v, touched);
    if (v.retry && !more) {
        // Rows that waited for their message to be read again (reconcile): every message is read now.
        v.retry = false;
        reconcileAll(v);
    }
    return changed;
}

void ChatTextReactions::publish(View& v)
{
    if (!m_hub || !v.browser)
        return;
    textreact::IndexedChat chat;
    chat.serverUid  = v.serverUid;
    chat.kind       = v.kind;
    chat.partnerUid = v.partnerUid;
    for (const Entry& e : qAsConst(v.entries)) {
        if (e.eligible)
            chat.messages.append({e.hash, e.timeSec, e.sender});
    }
    m_hub->index().setChat(reinterpret_cast<quintptr>(v.browser.data()), chat);
    v.published = true;
    m_hub->indexChanged();
}

void ChatTextReactions::afterScan(QTextBrowser* browser)
{
    View* v = viewFor(browser);
    if (!v)
        return;
    if (v->document != browser->document()) {
        // TeamSpeak swapped the document (a history reload): the old one, if it still exists, gets its
        // text back; the new one is read from scratch.
        if (v->document)
            giveBack(v->document.data(), v);
        v->entries.clear();
        v->days.clear();
        v->blocksByKey.clear();
        v->rendered.clear();
        v->document = browser->document();
    }
    classify(*v, false);
    const bool shown = Settings::instance().showReactions;
    if (shown != m_shown) {
        m_shown = shown;
        for (auto it = m_views.begin(); it != m_views.end(); ++it)
            reconcileAll(it.value());
    }
    if (index(*v) || !v->published)
        publish(*v);
}

// ---- rows ---------------------------------------------------------------------------------------------

bool ChatTextReactions::wantsRow(const View& v, int n) const
{
    if (!m_shown || !m_hub || v.kind == textreact::ChatKind::Server || n < 0 || n >= v.entries.size())
        return false;
    const Entry& e = v.entries.at(n);
    if (!e.eligible)
        return false;
    if (m_hub->store()->hasReactions(e.key))
        return true;
    return m_keep.contains(e.key) && m_hoverIn == v.browser && m_hoverBlock == n;
}

ChatTextReactions::Look ChatTextReactions::lookOf(QTextBrowser* browser) const
{
    const PreviewStyle style = m_chat->styleFor(browser);
    Look               look;
    look.dark     = style.dark;
    look.base     = browser->viewport()->palette().color(QPalette::Base);
    look.font     = style.font;
    look.dpr      = style.dpr;
    look.maxWidth = qMax(rx::kAddPillWidth, style.maxWidth);
    look.onBubble = !style.bubbles.isEmpty();
    return look;
}

rx::RowLayout ChatTextReactions::layoutFor(const View&, const QString& key, const Look& look, bool keep, bool mirrored) const
{
    if (!m_hub)
        return {};
    const QFontMetricsF   normal(rx::countFont(look.font, false));
    const QFontMetricsF   bold(rx::countFont(look.font, true));
    const rx::MeasureText measure = [&](const QString& text, bool isBold) { return (isBold ? bold : normal).horizontalAdvance(text); };
    const rx::RowLayout   row     = rx::layoutRow(m_hub->view(key), 0, look.maxWidth, measure, keep);
    // 2.3.1: right-aligned under right-to-left text of yours: the pills from the right, flush with the text (the add
    // pill's slot, empty until the row is hovered, on the left).
    return mirrored ? rx::mirrored(row) : row;
}

void ChatTextReactions::reconcileAll(View& v)
{
    QVector<int> blocks;
    if (v.browser)
        v.lookKey = lookKeyOf(lookOf(v.browser)); // every row is laid out for it now
    if (QTextDocument* doc = v.document) {
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const int n = b.blockNumber();
            if (n < v.entries.size() && v.entries.at(n).eligible) {
                blocks.append(n);
                continue;
            }
            const textreactdoc::Row row = textreactdoc::rowOf(b);
            if (row.exists() || row.separator >= 0)
                blocks.append(n); // one to take out
        }
    }
    reconcile(v, blocks);
}

void ChatTextReactions::reconcile(View& v, const QVector<int>& blocks)
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = v.document;
    if (!browser || !doc || doc != browser->document() || blocks.isEmpty())
        return;
    struct Edit {
        int     block = -1;
        int     kind  = 0; // 1 insert, 2 resize, 3 remove, 4 replace
        QString key;
        QSizeF  size;
        bool    current = false; // the block's entry was read from its text as it is now
    };
    QVector<Edit> edits;
    const Look    look = lookOf(browser);
    for (int n : blocks) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isValid())
            continue;
        const bool current = n < v.entries.size() && v.entries.at(n).read && v.entries.at(n).signature == signatureOf(b);
        const textreactdoc::Row row  = textreactdoc::rowOf(b);
        const bool              want = wantsRow(v, n);
        if (!want) {
            if (row.exists() || row.separator >= 0)
                edits.append({n, 3, QString(), QSizeF(), current});
            continue;
        }
        if (!current) {
            // TeamSpeak changed this chat since it was read (its line limit renumbers every block): no row is
            // put anywhere before the message is read again.
            v.retry = true;
            continue;
        }
        const QString       key    = v.entries.at(n).key;
        const bool          keep   = !m_hub->store()->hasReactions(key);
        const rx::RowLayout layout = layoutFor(v, key, look, keep);
        if (layout.isEmpty()) {
            if (row.exists())
                edits.append({n, 3, QString(), QSizeF(), current});
            continue;
        }
        const QSizeF size(layout.size);
        if (!row.exists())
            edits.append({n, 1, key, size, current});
        else if (row.key != key)
            edits.append({n, 4, key, size, current});
        else if (!qFuzzyCompare(row.format.width(), size.width()) || !qFuzzyCompare(row.format.height(), size.height()))
            edits.append({n, 2, key, size, current});
    }
    if (v.retry && v.browser) {
        m_indexViews.insert(v.browser.data());
        if (!m_indexTimer->isActive())
            m_indexTimer->start(kIndexStepMs);
    }
    if (!edits.isEmpty()) {
        const Place place = capturePlace(browser);
        {
            layoutformat::MutatingScope mutating; // no rescans, ChatEmoji and ChatLayout leave it alone
            QTextCursor                 batch(doc);
            batch.beginEditBlock();
            for (const Edit& e : qAsConst(edits)) {
                if (e.kind == 3 || e.kind == 4) {
                    const textreactdoc::Row row = textreactdoc::rowOf(doc->findBlockByNumber(e.block));
                    if (row.exists()) {
                        v.rendered.remove(row.format.name());
                        doc->addResource(QTextDocument::ImageResource, QUrl(row.format.name()), QVariant());
                    }
                    textreactdoc::removeRow(doc, e.block);
                }
                if (e.kind == 1 || e.kind == 4) {
                    const QString name = newName();
                    doc->addResource(QTextDocument::ImageResource, QUrl(name), m_blank);
                    textreactdoc::insertRow(doc, e.block, name, e.key, e.size);
                }
                if (e.kind == 2)
                    textreactdoc::resizeRow(doc, e.block, e.size);
            }
            batch.endEditBlock(); // TeamSpeak's view hears of it while we are still "mutating"
        }
        // What was read of those messages still holds: only our row changed in their text.
        for (const Edit& e : qAsConst(edits)) {
            if (e.current && e.block < v.entries.size())
                v.entries[e.block].signature = signatureOf(doc->findBlockByNumber(e.block));
        }
        restorePlace(browser, place);
        afterEdit(browser);
    }
    // The pictures of what changed and is on screen now (the rest when it comes into view).
    bool                  drawn = !edits.isEmpty();
    const QPair<int, int> shown = blocksShown(browser, kShownMargin);
    for (int n : blocks) {
        if (n >= shown.first && n <= shown.second)
            drawn = renderRow(v, n, false) || drawn;
    }
    if (drawn)
        browser->viewport()->update();
}

void ChatTextReactions::afterEdit(QTextBrowser* browser)
{
    // Positions after the rows moved: the previews' and the replies' indexes are read again.
    if (ChatIntegration::View* view = m_chat->viewFor(browser))
        view->positionsValid = false;
    m_chat->scheduleVisibilityUpdate();
    if (m_chat->m_replies)
        m_chat->m_replies->documentEdited(browser);
}

void ChatTextReactions::refreshKey(const QString& key)
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        const auto blocks = it->blocksByKey.constFind(key);
        if (blocks != it->blocksByKey.constEnd())
            reconcile(it.value(), blocks.value());
    }
}

QString ChatTextReactions::newName()
{
    return textreactdoc::objectPrefix() + m_tag + QLatin1Char('-') + QString::number(++m_serial);
}

QPair<int, int> ChatTextReactions::blocksShown(QTextBrowser* browser, qreal margin) const
{
    QTextDocument*               doc    = browser->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const qreal                  top    = browser->verticalScrollBar()->value() - margin;
    const qreal                  bottom = browser->verticalScrollBar()->value() + browser->viewport()->height() + margin;
    const int                    a      = layout->hitTest(QPointF(0, qMax<qreal>(0, top)), Qt::FuzzyHit);
    const int                    b      = layout->hitTest(QPointF(0, bottom), Qt::FuzzyHit);
    const int                    first  = a < 0 ? 0 : doc->findBlock(a).blockNumber();
    const int                    last   = b < 0 ? doc->blockCount() - 1 : doc->findBlock(b).blockNumber();
    return {first, qMax(first, last)};
}

void ChatTextReactions::renderVisible(View& v)
{
    QTextBrowser* browser = v.browser;
    if (!browser || !v.document || v.document != browser->document() || v.blocksByKey.isEmpty())
        return;
    // The chat's font or width changed without its text (TeamSpeak's font size, the layout's indent): the
    // rows are laid out again once that settles (not at every frame of a video playing in the chat).
    const QString lookKey = lookKeyOf(lookOf(browser));
    if (v.lookKey.isEmpty()) {
        v.lookKey = lookKey;
    } else if (v.lookKey != lookKey) {
        v.lookKey = lookKey;
        m_relayoutViews.insert(browser);
        m_relayout->start();
    }
    const QPair<int, int> shown = blocksShown(browser, kShownMargin);
    int                   onScreen = 0; // drawn rows in that range
    for (int n = shown.first; n <= shown.second && n < v.entries.size(); ++n) {
        if (!v.entries.at(n).eligible)
            continue;
        renderRow(v, n, false);
        const textreactdoc::Row row = textreactdoc::rowOf(v.document->findBlockByNumber(n));
        if (row.exists() && v.rendered.contains(row.format.name()))
            ++onScreen;
    }
    if (v.rendered.size() <= kMaxDrawn + onScreen)
        return;
    // Far off screen: the blank again (a row's picture is up to the chat's width at the screen's scale).
    // Pictures whose row went with TeamSpeak's text (a cleared chat, its line limit) are let go too, or
    // they would count here for ever and this would run on every paint.
    QTextDocument* doc = v.document;
    QSet<QString>  present;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const textreactdoc::Row row = textreactdoc::rowOf(b);
        if (!row.exists())
            continue;
        const QString name = row.format.name();
        present.insert(name);
        const int n = b.blockNumber();
        if (n >= shown.first && n <= shown.second)
            continue;
        if (v.rendered.size() > kMaxDrawn / 2 + onScreen && v.rendered.remove(name) > 0)
            doc->addResource(QTextDocument::ImageResource, QUrl(name), m_blank);
    }
    for (auto it = v.rendered.begin(); it != v.rendered.end();) {
        if (present.contains(it.key())) {
            ++it;
            continue;
        }
        doc->addResource(QTextDocument::ImageResource, QUrl(it.key()), QVariant());
        it = v.rendered.erase(it);
    }
}

bool ChatTextReactions::renderRow(View& v, int n, bool force)
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = v.document;
    if (!browser || !doc || !m_hub)
        return false;
    const QTextBlock        block = doc->findBlockByNumber(n);
    const textreactdoc::Row row   = textreactdoc::rowOf(block);
    if (!row.exists())
        return false;
    const Look         look     = lookOf(browser);
    const ReactionView view     = m_hub->view(row.key);
    const bool         keep     = view.isEmpty();
    const bool         mirrored = layoutformat::isOwnRightAligned(block); // 2.3.1
    rx::RowPaint       paint;
    paint.showAdd = m_hoverIn == browser && m_hoverBlock == n && canAddIn(v);
    if (m_hover.browser == browser && m_hover.block == n) {
        if (m_hover.zone == rx::Zone::Pill)
            paint.hoverReaction = m_hover.index;
        paint.addHover = m_hover.zone == rx::Zone::AddPill;
    }
    if (m_press.browser == browser && m_press.block == n && m_hover.same(m_press)) {
        if (m_press.zone == rx::Zone::Pill)
            paint.pressedReaction = m_press.index;
        paint.addPressed = m_press.zone == rx::Zone::AddPill;
    }
    QString signature = QString::fromLatin1("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                            .arg(row.key)
                            .arg((look.dark ? 1 : 0) + (look.onBubble ? 2 : 0) + (mirrored ? 4 : 0))
                            .arg(look.base.rgba())
                            .arg(look.dpr)
                            .arg(look.maxWidth)
                            .arg(look.font.key())
                            .arg(paint.showAdd ? 1 : 0)
                            .arg(paint.hoverReaction * 4 + (paint.addHover ? 1 : 0) + (paint.addPressed ? 2 : 0))
                            .arg(paint.pressedReaction);
    for (const ReactionView::Entry& e : view.entries)
        signature += QString::fromLatin1("|%1:%2:%3").arg(e.reaction).arg(e.count).arg(e.mine ? 1 : 0);
    const QString name = row.format.name();
    if (!force && v.rendered.value(name) == signature)
        return false;
    const rx::RowLayout layout = layoutFor(v, row.key, look, keep, mirrored);
    const QSize         size(qMax(1, qRound(row.format.width())), qMax(1, qRound(row.format.height())));
    QImage              image(size * look.dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(look.dpr);
    image.fill(Qt::transparent);
    if (!layout.isEmpty()) {
        QPainter p(&image);
        rx::drawRow(p, QPointF(0, 0), layout, view, paint, rx::rowColors(look.dark, look.base, look.onBubble), look.font);
        p.end();
    }
    doc->addResource(QTextDocument::ImageResource, QUrl(name), QPixmap::fromImage(image));
    v.rendered.insert(name, signature);
    return true;
}

void ChatTextReactions::redraw(QTextBrowser* browser, int block)
{
    View* v = viewFor(browser);
    if (!v || block < 0)
        return;
    renderRow(*v, block, false);
    const QRectF r = objectRect(browser, block);
    if (r.isValid())
        browser->viewport()->update(r.toAlignedRect().adjusted(-2, -2, 2, 2));
}

QRectF ChatTextReactions::objectRect(QTextBrowser* browser, int block) const
{
    const QRectF r = textreactdoc::rowRect(browser->document()->findBlockByNumber(block));
    return r.isValid() ? r.translated(-browser->horizontalScrollBar()->value(), -browser->verticalScrollBar()->value()) : QRectF();
}

// ---- the pointer --------------------------------------------------------------------------------------

int ChatTextReactions::blockAt(View& v, const QPoint& pos) const
{
    QTextBrowser*  browser = v.browser;
    QTextDocument* doc     = browser->document();
    const QPointF  docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return -1;
    const QTextBlock b = doc->findBlock(hit);
    if (!b.isValid())
        return -1;
    const QRectF r = doc->documentLayout()->blockBoundingRect(b);
    return docPos.y() >= r.top() && docPos.y() <= r.bottom() ? b.blockNumber() : -1;
}

ChatTextReactions::Hit ChatTextReactions::hitAt(View& v, const QPoint& pos) const
{
    Hit       hit;
    const int n = blockAt(v, pos);
    if (n < 0 || !m_hub)
        return hit;
    const QRectF rect = objectRect(v.browser, n);
    if (!rect.isValid() || !rect.contains(pos))
        return hit;
    const QTextBlock        block  = v.document->findBlockByNumber(n);
    const textreactdoc::Row row    = textreactdoc::rowOf(block);
    const Look              look   = lookOf(v.browser);
    const rx::RowLayout     layout = layoutFor(v, row.key, look, m_hub->view(row.key).isEmpty(), layoutformat::isOwnRightAligned(block));
    hit.block  = n;
    hit.object = rect;
    hit.zone   = rx::zoneAt(QSizeF(0, 0), layout, false, QPointF(pos) - rect.topLeft());
    if (hit.zone.zone == rx::Zone::None)
        hit.zone.zone = rx::Zone::Row;
    return hit;
}

void ChatTextReactions::setSpot(const Spot& spot)
{
    if (spot.same(m_hover))
        return;
    const Spot old = m_hover;
    m_hover        = spot;
    if (old.browser)
        redraw(old.browser, old.block);
    if (spot.browser && !(old.browser == spot.browser && old.block == spot.block))
        redraw(spot.browser, spot.block);
}

void ChatTextReactions::setHoverBlock(QTextBrowser* browser, int block)
{
    if (m_hoverIn == browser && m_hoverBlock == block)
        return;
    const QPointer<QTextBrowser> oldIn    = m_hoverIn;
    const int                    oldBlock = m_hoverBlock;
    m_hoverIn                             = browser;
    m_hoverBlock                          = block;
    if (oldIn) {
        // A row kept for the pointer goes once it leaves the message.
        View* v = viewFor(oldIn);
        if (v && oldBlock >= 0 && oldBlock < v->entries.size() && m_keep.remove(v->entries.at(oldBlock).key))
            reconcile(*v, {oldBlock});
        else
            redraw(oldIn, oldBlock); // its add pill goes
    }
    if (browser)
        redraw(browser, block);
}

bool ChatTextReactions::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::Paint && type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick
        && type != QEvent::MouseButtonRelease && type != QEvent::ToolTip && type != QEvent::Leave && type != QEvent::Resize)
        return false;
    View* v = viewForViewport(watched);
    if (!v || !v->browser || v->document != v->browser->document())
        return false;
    QTextBrowser* browser = v->browser;

    switch (type) {
    case QEvent::Paint:
        renderVisible(*v); // before the chat paints: the rows it is about to show
        // A chat shown with nothing that reads what it is (a private chat chosen in the tab bar while its
        // partner's reaction on your message waits, live test 2.3): read right after this paint, not in it,
        // so the partner is known within TextReactHub's wait.
        if ((v->kind == textreact::ChatKind::Unknown || (v->kind == textreact::ChatKind::Private && v->partnerUid.isEmpty()))
            && QDateTime::currentMSecsSinceEpoch() - v->kindCheckedMs >= kKindRecheckMs && !m_indexViews.contains(browser)) {
            m_indexViews.insert(browser);
            if (!m_indexTimer->isActive())
                m_indexTimer->start(kReadSoonMs);
        }
        return false;
    case QEvent::Resize:
        if (!v->blocksByKey.isEmpty()) {
            m_relayoutViews.insert(browser); // rows wrap at the new width, once resizing pauses
            m_relayout->start();
        }
        return false;
    case QEvent::Leave:
        setSpot(Spot());
        setHoverBlock(nullptr, -1);
        return false;
    default:
        break;
    }
    if (!m_hub || !m_shown) {
        setSpot(Spot());
        return false;
    }

    QPoint pos;
    if (type == QEvent::ToolTip)
        pos = static_cast<QHelpEvent*>(event)->pos();
    else
        pos = static_cast<QMouseEvent*>(event)->pos();
    if (type == QEvent::MouseMove && (m_chat->m_seeking || (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton && !m_press.browser)))
        return false; // a selection or a seek going on

    const Hit hit = hitAt(*v, pos);
    Spot      spot;
    if (reactive(hit.zone.zone)) {
        spot.browser = browser;
        spot.block   = hit.block;
        spot.zone    = hit.zone.zone;
        spot.index   = hit.zone.index;
    }

    switch (type) {
    case QEvent::MouseMove: {
        classify(*v, false);
        setHoverBlock(browser, blockAt(*v, pos));
        setSpot(spot);
        if (!reactive(hit.zone.zone))
            return false;
        m_chat->updateCursor(browser, true, pos, actionable(hit.zone.zone));
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        if (!reactive(hit.zone.zone))
            return false;
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && actionable(hit.zone.zone)) {
            m_press = spot;
            QToolTip::hideText();
            redraw(browser, hit.block);
        }
        return me->button() == Qt::LeftButton; // no text selection or drag from the row; the right button's menu opens
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || !m_press.browser)
            return false;
        const Spot pressed = m_press;
        m_press            = Spot();
        redraw(pressed.browser, pressed.block);
        if (pressed.same(spot)) {
            const textreactdoc::Row row = textreactdoc::rowOf(v->document->findBlockByNumber(spot.block));
            if (spot.zone == rx::Zone::Pill) {
                toggle(browser, row.key, spot.index);
            } else if (row.exists()) {
                const QRectF  local   = hit.zone.rect.translated(hit.object.topLeft());
                const QString blocked = blockedText(browser);
                if (blocked.isEmpty())
                    openPicker(browser, row.key, QRect(browser->viewport()->mapToGlobal(local.topLeft().toPoint()), local.size().toSize()));
                else
                    m_chat->showFeedback(browser->viewport(), blocked, true);
            }
        }
        return true;
    }
    case QEvent::ToolTip: {
        if (!reactive(hit.zone.zone))
            return false;
        auto*         he   = static_cast<QHelpEvent*>(event);
        const QRect   area = hit.zone.rect.translated(hit.object.topLeft()).toAlignedRect();
        const QString key  = textreactdoc::rowOf(v->document->findBlockByNumber(hit.block)).key;
        QString       text;
        if (hit.zone.zone == rx::Zone::Pill)
            text = pillToolTip(key, hit.zone.index);
        else if (hit.zone.zone == rx::Zone::AddPill)
            text = i18n::t("Add reaction\nSeen by people with TS Media 2.3 or later who are online in this chat.");
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(he->globalPos(), text, browser->viewport(), area);
        return true;
    }
    default:
        break;
    }
    return false;
}

// ---- acting -------------------------------------------------------------------------------------------

bool ChatTextReactions::canAddIn(const View& v) const
{
    PeerHub* peers = PeerHub::instance();
    if (!m_hub || !peers || (v.kind != textreact::ChatKind::Channel && v.kind != textreact::ChatKind::Private))
        return false;
    // Only a shown chat can be hovered: it belongs to the current server tab.
    const uint64 sch = ts3::currentConnection();
    return ts3::isConnected(sch) && peers->link() && !peers->link()->blocked(sch);
}

QString ChatTextReactions::blockedText(QTextBrowser* browser) const
{
    PeerHub* peers = PeerHub::instance();
    if (!peers || !m_hub)
        return i18n::t("Your reaction wasn't sent.");
    ChatTarget target;
    if (!m_chat->resolveTarget(browser, &target))
        return i18n::t("Can't tell who this private chat is with, so your reaction can't be delivered.");
    return PeerHub::errorText(peers->reactBlock(target));
}

// Read only (the action bar asks while ChatLayout handles a mouse move): a message that arrived since the
// last scan is read now, but nothing is changed.
QString ChatTextReactions::keyOf(QTextBrowser* browser, int block)
{
    View* v = viewFor(browser);
    if (!v || block < 0 || !v->document || v->document != browser->document())
        return {};
    const QTextBlock b = v->document->findBlockByNumber(block);
    if (!b.isValid())
        return {};
    const quint64 signature = signatureOf(b);
    if (block < v->entries.size() && v->entries.at(block).read && v->entries.at(block).signature == signature)
        return v->entries.at(block).eligible ? v->entries.at(block).key : QString();
    QVector<QDate> dates;
    bool           datesRead = false;
    const Entry    e         = readEntry(v->document, b, &dates, &datesRead, v->days, 0); // the days as the last reading numbered them
    if (block < v->entries.size()) {
        Entry& known = v->entries[block];
        if (known.eligible == e.eligible && known.key == e.key) {
            // Restyled only (the layout, HD emoji): the same message. Remembered, so the action bar (asked on
            // every mouse move) doesn't read it again; nothing else changes.
            known.signature = e.signature;
            known.read      = true;
        } else if (v->browser) {
            m_indexViews.insert(v->browser.data()); // something else: read in order soon
            if (!m_indexTimer->isActive())
                m_indexTimer->start(kIndexStepMs);
        }
    }
    return e.eligible ? e.key : QString();
}

bool ChatTextReactions::canReact(QTextBrowser* browser, int block)
{
    if (!m_shown || !m_hub || !browser)
        return false;
    View* v = viewFor(browser);
    if (!v)
        return false;
    classify(*v, false);
    return !keyOf(browser, block).isEmpty() && canAddIn(*v);
}

void ChatTextReactions::toggle(QTextBrowser* browser, const QString& key, int reaction)
{
    View* v = viewFor(browser);
    if (!m_hub || !v || key.isEmpty())
        return;
    ChatTarget target;
    if (!m_chat->resolveTarget(browser, &target)) {
        m_chat->showFeedback(browser->viewport(), i18n::t("Can't tell who this private chat is with, so your reaction can't be delivered."), true);
        return;
    }
    const TextReactHub::ReactError error = m_hub->toggle(key, reaction, target);
    if (error != TextReactHub::ReactError::None) {
        m_chat->showFeedback(browser->viewport(), PeerHub::errorText(error), true);
        return;
    }
    // Removing the last reaction while the pointer is on the message keeps the row until it leaves.
    const bool hovered = m_hoverIn == browser && m_hoverBlock >= 0 && m_hoverBlock < v->entries.size() && v->entries.at(m_hoverBlock).key == key;
    if (m_hub->view(key).isEmpty() && hovered)
        m_keep.insert(key);
    if (!v->blocksByKey.contains(key))
        afterScan(browser); // a message that just arrived (a click, not a hover: changing the chat is fine here)
    reconcile(*v, v->blocksByKey.value(key)); // at once; the other chats follow with the store's change
}

void ChatTextReactions::openPickerAt(QTextBrowser* browser, int block, const QRect& anchor)
{
    if (!browser)
        return;
    const QString key = canReact(browser, block) ? keyOf(browser, block) : QString();
    const QString blocked = blockedText(browser);
    if (!blocked.isEmpty() || key.isEmpty()) {
        m_chat->showFeedback(browser->viewport(), blocked, true);
        return;
    }
    openPicker(browser, key, anchor);
    if (m_picker)
        m_picker->setOpener(anchor); // the bar's button again closes it
}

void ChatTextReactions::openPicker(QTextBrowser* browser, const QString& key, const QRect& anchor)
{
    if (!m_hub || !browser || key.isEmpty())
        return;
    if (m_picker)
        m_picker->close();
    const Look               look = lookOf(browser);
    const proto::ReactionSet own  = m_hub->view(key).own();
    auto* picker = new ReactionPicker(look.dark, look.base, emoji::prefs::quickReactions(), QSet<int>(own.begin(), own.end()));
    m_picker     = picker;
    QPointer<QTextBrowser> guard(browser);
    // By the message's key: blocks may have moved by the time the pick comes.
    connect(picker, &ReactionPicker::picked, this, [this, guard, key](int reaction) {
        if (guard)
            toggle(guard.data(), key, reaction);
    });
    connect(picker, &ReactionPicker::more, this, [this, guard, key, anchor] {
        if (guard)
            openFullPicker(guard.data(), key, anchor);
    });
    picker->openAt(anchor);
}

void ChatTextReactions::openFullPicker(QTextBrowser* browser, const QString& key, const QRect& anchor)
{
    if (!m_hub || !browser)
        return;
    if (m_fullPicker)
        m_fullPicker->close();
    const Look               look   = lookOf(browser);
    const proto::ReactionSet own    = m_hub->view(key).own();
    auto*                    picker = new EmojiPicker(EmojiPicker::Mode::React, look.dark, look.base);
    picker->setMarked(QSet<int>(own.begin(), own.end()));
    m_fullPicker = picker;
    QPointer<QTextBrowser> guard(browser);
    QPointer<EmojiPicker>  pickerGuard(picker);
    connect(picker, &EmojiPicker::picked, this, [this, guard, key, pickerGuard](int reaction, bool) {
        if (!guard)
            return;
        toggle(guard.data(), key, reaction);
        if (pickerGuard && m_hub) {
            const proto::ReactionSet now = m_hub->view(key).own();
            pickerGuard->setMarked(QSet<int>(now.begin(), now.end()));
        }
    });
    picker->setOpener(anchor);
    picker->openAt(anchor, true);
}

QAction* ChatTextReactions::menuAction(QMenu* menu, QTextBrowser* browser, int block)
{
    if (!menu || !browser || !m_shown || !m_hub)
        return nullptr;
    View* v = viewFor(browser);
    if (!v)
        return nullptr;
    classify(*v, true);
    const QString key = keyOf(browser, block);
    if (key.isEmpty() || (v->kind != textreact::ChatKind::Channel && v->kind != textreact::ChatKind::Private))
        return nullptr;
    auto* action = new QAction(addIcon(menu->devicePixelRatioF()), i18n::t("Add reaction…"), menu);
    const QString blocked = blockedText(browser);
    if (!blocked.isEmpty()) {
        // Shown, but disabled with the reason (rather than missing without one).
        action->setEnabled(false);
        action->setToolTip(blocked);
        menu->setToolTipsVisible(true);
        return action;
    }
    QPointer<QTextBrowser> guard(browser);
    connect(action, &QAction::triggered, this, [this, guard, key] {
        if (guard)
            openPicker(guard.data(), key, QRect(QCursor::pos(), QSize(1, 1)));
    });
    return action;
}

QString ChatTextReactions::pillToolTip(const QString& key, int reaction) const
{
    if (!m_hub || !emoji::isValid(reaction))
        return {};
    const ReactionView::Entry entry = m_hub->view(key).entry(reaction);
    if (entry.count <= 0)
        return {};
    QStringList names;
    if (entry.mine)
        names << i18n::t("you");
    names << entry.others;
    const QString name = rx::reactionName(reaction);
    QStringList   lines{i18n::t("%1: %2").arg(name, nameList(names, entry.count))};
    lines << (entry.mine ? i18n::t("Click to remove your reaction") : i18n::t("Click to react with %1").arg(name));
    return tipHtml(lines);
}

// ---- tests and the live-test driver ---------------------------------------------------------------------

int ChatTextReactions::rowCount(QTextBrowser* browser) const
{
    if (!browser)
        return 0;
    int count = 0;
    for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next())
        count += textreactdoc::rowOf(b).exists() ? 1 : 0;
    return count;
}

QRect ChatTextReactions::pillRect(QTextBrowser* browser, int block, int reaction)
{
    View* v = viewFor(browser);
    if (!v || !m_hub)
        return {};
    const QRectF rect = objectRect(browser, block);
    if (!rect.isValid())
        return {};
    const QTextBlock        b      = browser->document()->findBlockByNumber(block);
    const textreactdoc::Row row    = textreactdoc::rowOf(b);
    const rx::RowLayout     layout = layoutFor(*v, row.key, lookOf(browser), m_hub->view(row.key).isEmpty(), layoutformat::isOwnRightAligned(b));
    for (int i = 0; i < layout.pills.size(); ++i) {
        if (layout.reactions.at(i) == reaction)
            return layout.pills.at(i).translated(rect.topLeft()).toAlignedRect();
    }
    return {};
}
