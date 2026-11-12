#include "chatscale.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <limits>

#include "layoutformat.h"

ChatScale::ChatScale(QTextBrowser* browser, Host host, QObject* parent)
    : QObject(parent)
    , m_browser(browser)
    , m_host(std::move(host))
{
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(kSettleMs);
    connect(m_settle, &QTimer::timeout, this, &ChatScale::settle);
    m_hold = new QTimer(this);
    m_hold->setSingleShot(true);
    connect(m_hold, &QTimer::timeout, this, &ChatScale::hold);
    m_budgetTimer = new QTimer(this);
    m_budgetTimer->setSingleShot(true);
    m_budgetTimer->setInterval(0);
    connect(m_budgetTimer, &QTimer::timeout, this, [this] { enforceBudget(); });
    if (browser)
        browser->installEventFilter(this); // the chat widget itself: FontChange comes there before its document gets the font
}

ChatScale::~ChatScale()
{
    prepareShutdown();
}

void ChatScale::prepareShutdown()
{
    m_down = true;
    m_settle->stop();
    m_hold->stop();
    m_budgetTimer->stop();
    m_burst = false;
    if (m_browser)
        m_browser->removeEventFilter(this);
}

bool ChatScale::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_down && watched == m_browser && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) && m_host.fontEvent)
        m_host.fontEvent();
    return QObject::eventFilter(watched, event);
}

bool ChatScale::pending() const
{
    return m_settle->isActive();
}

void ChatScale::request()
{
    if (m_down || !m_browser)
        return;
    if (!m_burst) {
        // The first event of a burst: where the reader is, before Qt lays the chat out with the new font (the block
        // and how far into it, which survives a uniform re-scale; the bottom flag).
        m_place = chatscroll::capture(m_browser);
        m_burst = true;
    }
    m_settle->start(); // every event restarts it: one pass per gesture
    // During the gesture the line being read stays at the top (TeamSpeak alone lets it drift by the scroll value in
    // px): one cheap, anchor-only hold at most every kHoldMs. A chat at the bottom is placed once, at settle.
    if (!m_place.bottom && !m_hold->isActive()) {
        const qint64 since = m_lastHold.isValid() ? m_lastHold.elapsed() : kHoldMs;
        m_hold->start(static_cast<int>(qBound<qint64>(0, kHoldMs - since, kHoldMs)));
    }
}

void ChatScale::hold()
{
    if (m_down || !m_browser || !m_burst)
        return;
    m_lastHold.restart();
    ++m_holds;
    chatscroll::restoreAnchor(m_browser, m_place);
}

void ChatScale::settle()
{
    if (m_down || !m_browser)
        return;
    m_settle->stop();
    m_hold->stop();
    QTextBrowser* browser = m_browser;
    if (!m_burst) {
        m_place = chatscroll::capture(browser);
        m_burst = true;
    }
    const PreviewScale next    = m_host.compute ? m_host.compute() : m_scale;
    const bool         changed = !next.sameAs(m_scale);
    ++m_passes;
    QElapsedTimer took; // the log says how long the pass held the GUI thread, in its three parts
    took.start();
    qint64 applied = 0, placed = 0;
    if (changed) {
        m_scale = next;
        if (m_host.apply) {
            // One edit block: the document is laid out once for every new size. Our own edits (the scope spans the
            // block's end, where the chat hears of them): no rescan; ChatLayout fits what it must (bubbles on the right).
            layoutformat::MutatingScope mutating;
            QTextCursor                 batch(browser->document());
            batch.beginEditBlock();
            m_host.apply(next);
            batch.endEditBlock();
        }
        if (m_host.changed)
            m_host.changed(next);
    }
    applied = took.elapsed();
    // The full restore once (it lays the chat out to know its range): at the bottom again, or the block being read.
    chatscroll::restore(browser, m_place);
    m_burst = false;
    placed = took.elapsed();
    if (changed && m_host.after && m_browser)
        m_host.after();
    if (changed && m_host.log && !m_down)
        m_host.log(QStringLiteral("pass %1 ms (sizes %2, place %3, drawing %4)").arg(took.elapsed()).arg(applied).arg(placed - applied).arg(took.elapsed() - placed));
}

// ---- the picture budget -----------------------------------------------------------------------------------------------

QPixmap ChatScale::blankPicture()
{
    QPixmap blank(1, 1);
    blank.fill(Qt::transparent);
    return blank;
}

void ChatScale::putPicture(const QString& name, const QPixmap& pixmap, int module)
{
    if (!m_browser || name.isEmpty())
        return;
    m_browser->document()->addResource(QTextDocument::ImageResource, QUrl(name), pixmap);
    const bool known  = m_pictures.contains(name);
    Picture&   pic    = m_pictures[name];
    const qint64 before = pic.bytes;
    m_total -= pic.bytes;
    pic.bytes   = pixmap.isNull() ? 0 : static_cast<qint64>(pixmap.width()) * pixmap.height() * 4;
    pic.module  = module;
    pic.swapped = false;
    m_total += pic.bytes;
    // Only a put that grew the books looks again (a GIF's or a video's next frame puts the same bytes, up to 30 times a
    // second: no walk of the whole document for it).
    const bool grew = !known || pic.bytes > before;
    if (grew)
        m_stuck = false;
    if (grew && m_total > m_budget && !m_down && !m_budgetTimer->isActive())
        m_budgetTimer->start(); // coalesced: one look after this turn of the event loop
}

void ChatScale::resetLedger()
{
    m_pictures.clear();
    m_total      = 0;
    m_stuck      = false;
    m_overLogged = false;
}

void ChatScale::forget(const QString& name)
{
    const auto it = m_pictures.find(name);
    if (it == m_pictures.end())
        return;
    m_total -= it->bytes;
    m_pictures.erase(it);
}

bool ChatScale::isSwapped(const QString& name) const
{
    const auto it = m_pictures.constFind(name);
    return it != m_pictures.constEnd() && it->swapped;
}

int ChatScale::enforceBudget()
{
    if (m_down || !m_browser || m_total <= m_budget)
        return 0;
    QTextBrowser*                browser = m_browser;
    QTextDocument*               doc     = browser->document();
    QAbstractTextDocumentLayout* layout  = doc->documentLayout();
    const int                    scroll  = browser->verticalScrollBar()->value();
    if (m_stuck && doc->revision() == m_stuckRevision && scroll == m_stuckScroll && m_total <= m_stuckTotal)
        return 0; // nothing changed since a scan that couldn't do more
    ++m_scans;
    QElapsedTimer took; // the log says how long a scan held the GUI thread
    took.start();
    const qreal                  top     = scroll;
    const qreal                  bottom  = top + browser->viewport()->height();
    struct Candidate {
        QString name;
        qreal   distance = 0;
        qint64  bytes    = 0;
    };
    QVector<Candidate> candidates;
    QSet<QString>      seen;
    QSet<QString>      inDocument; // every name a fragment shows
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !f.charFormat().isImageFormat())
                continue;
            const QString name = f.charFormat().toImageFormat().name();
            inDocument.insert(name);
            const auto    pic  = m_pictures.constFind(name);
            if (pic == m_pictures.constEnd() || pic->swapped || pic->bytes <= 4 || seen.contains(name))
                continue;
            // Where it is: its line in the laid-out block (a block not laid out yet is laid out now).
            const QRectF br = layout->blockBoundingRect(block);
            qreal        y0 = br.top(), y1 = br.bottom();
            if (const QTextLayout* tl = block.layout()) {
                const QTextLine line = tl->lineForTextPosition(f.position() - block.position());
                if (line.isValid()) {
                    y0 = br.top() + line.y();
                    y1 = y0 + line.height();
                }
            }
            const qreal distance = y1 < top ? top - y1 : (y0 > bottom ? y0 - bottom : 0.0);
            // A picture of ours may stand in several places (the same file twice): the nearest counts.
            if (distance <= kSwapMargin) {
                seen.insert(name);
                for (int i = candidates.size() - 1; i >= 0; --i) {
                    if (candidates.at(i).name == name)
                        candidates.removeAt(i);
                }
                continue;
            }
            bool known = false;
            for (Candidate& c : candidates) {
                if (c.name == name) {
                    c.distance = qMin(c.distance, distance);
                    known      = true;
                }
            }
            if (!known)
                candidates.append({name, distance, pic->bytes});
        }
    }
    // Booked pictures no fragment shows any more (trimmed by the line limit, a preview that went without a forget):
    // the farthest of all. Their stand-in frees the document's copy (its resource map keeps it otherwise).
    for (auto it = m_pictures.constBegin(); it != m_pictures.constEnd(); ++it) {
        if (!it->swapped && it->bytes > 4 && !inDocument.contains(it.key()))
            candidates.append({it.key(), std::numeric_limits<qreal>::max(), it->bytes});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance > b.distance; });
    const qint64 target  = m_budget / 4 * 3;
    int          swapped = 0;
    for (const Candidate& c : qAsConst(candidates)) {
        if (m_total <= target)
            break;
        Picture& pic = m_pictures[c.name];
        doc->addResource(QTextDocument::ImageResource, QUrl(c.name), blankPicture()); // the format keeps its size: nothing moves
        m_total -= pic.bytes;
        pic.bytes   = 0;
        pic.swapped = true;
        ++swapped;
        if (m_host.swapped)
            m_host.swapped(c.name, pic.module);
    }
    // Still over the target: only pictures near the view are left. Not scanned again until something changes.
    m_stuck = m_total > target;
    if (m_stuck) {
        m_stuckRevision = doc->revision();
        m_stuckScroll   = scroll;
        m_stuckTotal    = m_total;
    }
    if (m_host.log) {
        const QString mib = QString::number(static_cast<double>(m_total) / (1024.0 * 1024.0), 'f', 1);
        if (swapped > 0)
            m_host.log(QStringLiteral("pictures %1 MiB swapped %2 in %3 ms").arg(mib).arg(swapped).arg(took.elapsed()));
        if (m_total > m_budget && !m_overLogged) {
            m_overLogged = true; // only pictures near the view are left: the budget is exceeded (once per chat)
            m_host.log(QStringLiteral("pictures %1 MiB near the view are over the budget").arg(mib));
        }
    }
    return swapped;
}
