// 2.2 album grid (feature 5): the chat side of albums. ChatIntegration's members for grouping the album
// items of chat messages into one grid each, keeping the chat documents in step (grid objects, collapsed
// item links, hidden follow-up messages), drawing the grids and handling the pointer per tile.

#include "chatintegration.h"

#include <QPixmap>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#include "albums.h"
#include "chatlayout.h"   // chat redesign: albums show or hide messages
#include "i18n.h"
#include "layoutformat.h" // chat redesign: headers kept in TS Media's chat layout
#include "chatreactions.h" // 2.2 reactions: the album's row
#include "inlinemedia.h"
#include "ownedtimer.h"
#include "settings.h"
#include "spoiler.h" // 2.2 spoiler: tiles under the shared cover

namespace {

// Block format of a message we hid (a later message of an album sent as several messages).
constexpr int kHiddenBlockProperty = QTextFormat::UserProperty + 0x7454;
// Char format of a collapsed item link: its text, and the line breaks that were in front of it.
constexpr int kCollapsedLabelProperty     = QTextFormat::UserProperty + 0x7455;
constexpr int kCollapsedSeparatorProperty = QTextFormat::UserProperty + 0x7456;
// What a collapsed link becomes: one zero-width character (WORD JOINER) that keeps the link's format.
constexpr ushort kCollapsedChar = 0x2060;
// Longer link texts are left alone (a link label is a file name: far shorter).
constexpr int kMaxCollapsedLabel = 1024;
constexpr int kAlbumFrameMs      = 33; // album redraws for GIF frames: at most ~30 per second

bool isGridKind(MediaKind kind)
{
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

QTextCharFormat charFormatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

// A grid (or a single preview) with the line breaks insertPreview put around it.
void removeObjectAt(QTextDocument* doc, int position, bool ownSeparatorAfter)
{
    const int   from = position > 0 && doc->characterAt(position - 1) == QChar::LineSeparator ? position - 1 : position;
    const int   to   = ownSeparatorAfter ? position + 2 : position + 1;
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.removeSelectedText();
}

// The link at [start, end) and the line breaks in front of it ([from, start)) become one zero-width
// character with the link's format; its text and the line breaks are kept in that format.
void collapseLink(QTextDocument* doc, int from, int start, int end)
{
    QString separator;
    QString label;
    for (int p = from; p < start; ++p)
        separator += doc->characterAt(p);
    for (int p = start; p < end; ++p)
        label += doc->characterAt(p);
    QTextCharFormat marker = charFormatAt(doc, start);
    marker.setProperty(kCollapsedLabelProperty, label);
    marker.setProperty(kCollapsedSeparatorProperty, separator);
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.insertText(QString(QChar(kCollapsedChar)), marker);
}

// Gives a collapsed link its line breaks and text back.
void restoreLink(QTextDocument* doc, int position)
{
    const QTextCharFormat marker = charFormatAt(doc, position);
    if (!marker.hasProperty(kCollapsedLabelProperty) || doc->characterAt(position) != QChar(kCollapsedChar))
        return;
    QString         label     = marker.stringProperty(kCollapsedLabelProperty);
    const QString   separator = marker.stringProperty(kCollapsedSeparatorProperty);
    QTextCharFormat link      = marker;
    link.clearProperty(kCollapsedLabelProperty);
    link.clearProperty(kCollapsedSeparatorProperty);
    QTextCharFormat plain = link;
    plain.setAnchor(false);
    plain.clearProperty(QTextFormat::AnchorHref);
    plain.clearProperty(QTextFormat::AnchorName);
    if (label.isEmpty())
        label = QString(QChar(0x2026));
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    if (!separator.isEmpty())
        c.insertText(separator, plain);
    c.insertText(label, link);
}

void setBlockHidden(QTextDocument* doc, int number, bool hidden)
{
    QTextBlock block = doc->findBlockByNumber(number);
    if (!block.isValid())
        return;
    QTextBlockFormat format = block.blockFormat();
    if (hidden)
        format.setProperty(kHiddenBlockProperty, true);
    else
        format.clearProperty(kHiddenBlockProperty);
    QTextCursor c(block);
    c.setBlockFormat(format);
    block = doc->findBlockByNumber(number);
    block.setVisible(!hidden);
    // S0: TeamSpeak's chat uses the stock QTextDocumentLayout, which honours this (height 0).
    doc->markContentsDirty(block.position(), block.length());
}

// The geometry of a grid drawn at rect's size: albums::layout() for its width (the grid is always as
// wide as its limit) and the preview height setting.
albums::Geometry geometryFor(int items, const QRectF& rect)
{
    return albums::layout(items, qRound(rect.width()), Settings::instance().previewMaxHeight);
}

// A tile of g mapped to rect (the grid as it is shown; normally the same size as g.box).
QRectF tileRectIn(const albums::Geometry& g, int tile, const QRectF& rect)
{
    const QRectF t(g.tiles.at(tile));
    const qreal  sx = g.box.width() > 0 ? rect.width() / g.box.width() : 1.0;
    const qreal  sy = g.box.height() > 0 ? rect.height() / g.box.height() : 1.0;
    return QRectF(rect.left() + t.left() * sx, rect.top() + t.top() * sy, t.width() * sx, t.height() * sy);
}

} // namespace

// ============================================================================================
// Planning: which links of the chat are albums
// ============================================================================================

QStringList ChatIntegration::albumKeysOf(const QString& id)
{
    QStringList keys;
    albums::parseObjectId(id, nullptr, &keys);
    return keys;
}

ChatIntegration::AlbumPlan ChatIntegration::planAlbums(QTextBrowser* browser) const
{
    AlbumPlan      plan;
    QTextDocument* doc = browser->document();
    const bool     on  = Settings::instance().inlinePreviews && albums::enabled();

    struct LinkAt {
        int       start  = 0;
        int       end    = 0;
        QString   key;
        bool      marker = false;
    };
    struct BlockAt {
        int                          number = 0;
        QVector<LinkAt>              links;
        QHash<QString, QVector<int>> singles; // key -> positions of its single previews in this message
    };
    QVector<BlockAt>         blocks;
    QVector<albums::Message> messages;

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        BlockAt         at;
        albums::Message message;
        at.number = block.blockNumber();
        if (block.blockFormat().boolProperty(kHiddenBlockProperty)) {
            plan.tagged.insert(at.number);
            if (!block.isVisible())
                plan.hidden.insert(at.number);
        }

        QVector<MediaLink> parsed;
        QString            lastHref;
        int                lastEnd   = -1;
        int                nickStart = -1;
        int                nickEnd   = -1;
        QString            nickHref;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            const QTextCharFormat cf    = f.charFormat();
            const int             start = f.position();
            const int             end   = start + f.length();
            const QString         id    = objectIdOf(cf);
            if (layoutformat::isOurs(cf))
                continue; // chat redesign: our head keeps TeamSpeak's header (read below)
            if (!id.isEmpty()) {
                for (int p = start; p < end; ++p) {
                    if (albums::isObjectId(id))
                        plan.existing.insert(p, id);
                    else
                        at.singles[id].append(p);
                }
                continue;
            }
            // The sender's nickname link of TeamSpeak's header: the first client:// link, before any file.
            if (cf.isAnchor() && at.links.isEmpty() && cf.anchorHref().startsWith(QLatin1String("client://"), Qt::CaseInsensitive)) {
                if (nickStart < 0) {
                    nickStart = start;
                    nickEnd   = end;
                    nickHref  = cf.anchorHref();
                } else if (cf.anchorHref() == nickHref && start == nickEnd) {
                    nickEnd = end;
                }
                continue;
            }
            if (!isFileAnchor(cf))
                continue;
            const QString href = cf.anchorHref();
            if (href == lastHref && start == lastEnd && !at.links.isEmpty()) {
                at.links.last().end = end; // one link over several fragments
                lastEnd             = end;
                continue;
            }
            lastHref = href;
            lastEnd  = end;
            const MediaLink link = MediaLink::parse(href);
            LinkAt          l;
            l.start  = start;
            l.end    = end;
            l.key    = link.isValid() ? link.key() : QString();
            l.marker = cf.hasProperty(kCollapsedLabelProperty);
            if (l.marker)
                plan.markers.append(start);
            at.links.append(l);
            parsed.append(link);
        }

        // Album items: TS Media pictures and videos with valid album fields (the parser checked them).
        bool anyItem = false;
        for (int i = 0; i < at.links.size(); ++i) {
            const MediaLink& link = parsed.at(i);
            albums::Link     item;
            item.key = at.links.at(i).key;
            if (on && link.isValid() && link.hasAlbum() && isGridKind(kindForFileName(link.fileName)) && !testShowsRawChat(link)) {
                item.albumId = link.albumId;
                item.index   = link.albumIndex;
                item.count   = link.albumCount;
                anyItem      = true;
            }
            message.links.append(item);
        }
        if (anyItem) {
            // Who sent it: TeamSpeak's header link carries the unique id (S0); else what Core saw when the
            // message arrived (or what we posted); else the nickname (after a plugin reload, an unknown
            // header format). A bare message is nothing but the header in front of its first link.
            const QString  text      = block.text();
            const int      firstLink = at.links.first().start - block.position();
            albums::Header header;
            // Chat redesign: a block in TS Media's chat layout keeps TeamSpeak's header in our picture.
            const layoutformat::Header styled = layoutformat::headerOf(block);
            if (styled.valid) {
                const int body = styled.bodyStart - block.position();
                header         = albums::parseHeader(styled.lead, styled.nickText, styled.nickHref, styled.after + text.mid(body, qMax(0, firstLink - body)));
                message.bare   = header.valid && body <= firstLink && text.mid(body, firstLink - body).trimmed().isEmpty();
            } else {
                if (nickStart >= 0 && nickEnd - block.position() <= firstLink) {
                    const int from = nickStart - block.position();
                    const int to   = nickEnd - block.position();
                    header         = albums::parseHeader(text.left(from), text.mid(from, to - from), nickHref, text.mid(to, firstLink - to));
                }
                if (!header.valid)
                    header = albums::parseHeaderText(text.left(firstLink));
                message.bare = header.valid && header.length <= firstLink && text.mid(header.length, firstLink - header.length).trimmed().isEmpty();
            }
            if (!header.uid.isEmpty())
                message.sender = QStringLiteral("u:") + header.uid;
            for (int i = 0; i < message.links.size() && message.sender.isEmpty(); ++i) {
                const albums::Link& item = message.links.at(i);
                if (item.albumId == 0)
                    continue;
                const QString uid = m_core->albumSender(parsed.at(i).serverUid, item.albumId, item.index, item.key);
                if (!uid.isEmpty())
                    message.sender = QStringLiteral("u:") + uid;
            }
            if (message.sender.isEmpty() && !header.nick.isEmpty())
                message.sender = QStringLiteral("n:") + header.nick;
        }
        blocks.append(at);
        messages.append(message);
    }

    const QVector<albums::Album> found = on ? albums::group(messages) : QVector<albums::Album>();
    QSet<int>                    wantedMarkers;
    // scan() gives no link to these files in their message a preview; their single previews there are
    // stale (below), so the two agree and nothing is put in and taken out again on every scan.
    const QHash<int, QSet<QString>> memberKeys = albums::memberKeys(found, messages);
    for (auto it = memberKeys.constBegin(); it != memberKeys.constEnd(); ++it)
        plan.memberKeys.insert(blocks.at(it.key()).number, it.value());
    for (const albums::Album& album : found) {
        const BlockAt&    anchor = blocks.at(album.anchorMessage);
        AlbumPlan::Object object;
        object.id    = albums::objectId(album.albumId, album.keys);
        object.block = anchor.number;
        object.after = anchor.links.at(album.anchorLink).end;

        QVector<int> inAnchor; // the item links of the anchor message, in order
        for (const auto& member : album.members) {
            const BlockAt& at   = blocks.at(member.first);
            const LinkAt&  link = at.links.at(member.second);
            plan.members.insert(link.start);
            if (member.first == album.anchorMessage)
                inAnchor.append(member.second);
            for (const int position : at.singles.value(link.key))
                plan.staleSingles.append(position);
        }
        std::sort(inAnchor.begin(), inAnchor.end());
        // The first item's link stays; the others are collapsed when only line breaks separate them.
        for (int i = 1; i < inAnchor.size(); ++i) {
            const LinkAt& link = anchor.links.at(inAnchor.at(i));
            if (link.marker) {
                wantedMarkers.insert(link.start);
                continue;
            }
            const LinkAt& previous    = anchor.links.at(inAnchor.at(i - 1));
            bool          onlyBreaks  = link.end - link.start <= kMaxCollapsedLabel;
            for (int p = previous.end; p < link.start && onlyBreaks; ++p)
                onlyBreaks = doc->characterAt(p) == QChar::LineSeparator;
            if (onlyBreaks)
                object.collapse.append({previous.end, link.start, link.end});
        }
        plan.objects.append(object);
        for (const int message : album.hiddenMessages)
            plan.hide.insert(blocks.at(message).number);
    }
    for (const int marker : qAsConst(plan.markers)) {
        if (!wantedMarkers.contains(marker))
            plan.restore.append(marker);
    }
    std::sort(plan.staleSingles.begin(), plan.staleSingles.end());
    plan.staleSingles.erase(std::unique(plan.staleSingles.begin(), plan.staleSingles.end()), plan.staleSingles.end());
    plan.active = !found.isEmpty() || !plan.existing.isEmpty() || !plan.markers.isEmpty() || !plan.tagged.isEmpty();
    return plan;
}

// ============================================================================================
// Keeping the document in step
// ============================================================================================

void ChatIntegration::applyAlbums(QTextBrowser* browser, const AlbumPlan& plan)
{
    View* view = viewFor(browser);
    if (!view || !plan.active)
        return;
    QTextDocument* doc    = browser->document();
    const bool     bottom = atBottom(browser);

    enum class Kind { Remove, Collapse, Restore, Insert };
    struct Edit {
        Kind    kind;
        int     position = 0;
        int     start    = 0;
        int     end      = 0;
        QString id;
    };
    QVector<Edit> edits;
    QStringList   freed; // album objects taken out: their pictures are dropped from the document
    bool          again = false;

    if (!plan.staleSingles.isEmpty()) {
        // Previews of album items from before they were grouped (a plugin reload, an update from 2.1):
        // taken out first; the grid comes with the next scan.
        for (const int position : plan.staleSingles)
            edits.append({Kind::Remove, position, 0, 0, QString()});
        again = true;
    } else {
        // Per grid, not per message: a message can hold two (albums::gridEdits).
        QVector<albums::GridPlace> existing;
        QVector<int>               existingAt; // their positions
        for (auto it = plan.existing.constBegin(); it != plan.existing.constEnd(); ++it) {
            existing.append({doc->findBlock(it.key()).blockNumber(), it.value()});
            existingAt.append(it.key());
        }
        QVector<albums::GridPlace> wanted;
        for (const AlbumPlan::Object& object : plan.objects)
            wanted.append({object.block, object.id});
        const albums::GridEdits grids = albums::gridEdits(existing, wanted);
        for (const int i : grids.remove) {
            edits.append({Kind::Remove, existingAt.at(i), 0, 0, QString()});
            freed.append(existing.at(i).id);
        }
        for (const int i : grids.insert)
            edits.append({Kind::Insert, plan.objects.at(i).after, 0, 0, plan.objects.at(i).id});
        for (const AlbumPlan::Object& object : plan.objects) {
            for (const AlbumPlan::Collapse& c : object.collapse)
                edits.append({Kind::Collapse, c.from, c.start, c.end, QString()});
        }
        for (const int position : plan.restore)
            edits.append({Kind::Restore, position, 0, 0, QString()});
    }

    // Back to front, so the positions of the edits still to come stay valid; at one position, what is
    // taken out goes before what is put in.
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        if (a.position != b.position)
            return a.position > b.position;
        return (a.kind == Kind::Insert ? 1 : 0) < (b.kind == Kind::Insert ? 1 : 0);
    });

    // To hide: what isn't hidden yet. To show: everything tagged that shouldn't be (also a message that
    // inherited the tag from the hidden one before it when it was appended).
    QSet<int> hide   = plan.hide;
    QSet<int> unhide = plan.tagged;
    hide.subtract(plan.hidden);
    unhide.subtract(plan.hide);
    if (edits.isEmpty() && hide.isEmpty() && unhide.isEmpty()) {
        // Nothing to change; grids kept from an earlier plugin instance still need their pictures.
        ensurePositions(*view);
        for (auto it = view->positionsByKey.constBegin(); it != view->positionsByKey.constEnd(); ++it) {
            if (albums::isObjectId(it.key()) && !view->resourced.contains(it.key()))
                refreshPreview(browser, it.key(), false);
        }
        return;
    }

    m_mutating = true;
    for (const Edit& edit : qAsConst(edits)) {
        switch (edit.kind) {
        case Kind::Remove:
            removeObjectAt(doc, edit.position, isOwnSeparator(doc, edit.position + 1));
            break;
        case Kind::Collapse:
            collapseLink(doc, edit.position, edit.start, edit.end);
            break;
        case Kind::Restore:
            restoreLink(doc, edit.position);
            break;
        case Kind::Insert:
            insertPreview(browser, edit.position, edit.id); // draws it (renderFor -> renderAlbumFor)
            m_mutating = true;
            break;
        }
    }
    for (const int number : qAsConst(hide))
        setBlockHidden(doc, number, true);
    for (const int number : qAsConst(unhide))
        setBlockHidden(doc, number, false);
    m_mutating = false;
    if (m_layout && (!hide.isEmpty() || !unhide.isEmpty())) // chat redesign: the next visible message is grouped again
        m_layout->visibilityChanged(browser, hide + unhide);

    QSet<QString> stillShown;
    for (const AlbumPlan::Object& object : plan.objects)
        stillShown.insert(object.id);
    for (const QString& id : qAsConst(freed)) {
        if (stillShown.contains(id))
            continue;
        doc->addResource(QTextDocument::ImageResource, QUrl(resourceName(id)), QPixmap());
        view->resourced.remove(id);
    }
    view->positionsValid = false;
    ensurePositions(*view);
    for (auto it = view->positionsByKey.constBegin(); it != view->positionsByKey.constEnd(); ++it) {
        if (albums::isObjectId(it.key()) && !view->resourced.contains(it.key()))
            refreshPreview(browser, it.key(), false);
    }
    // S0: hiding a block doesn't keep the chat pinned to its end.
    if (bottom)
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QStringLiteral("[test] albums: %1 edit(s), %2 message(s) hidden, %3 shown again").arg(edits.size()).arg(hide.size()).arg(unhide.size()));
#endif
    requestSnapshot(browser, QStringLiteral("albums updated"));
    scheduleVisibilityUpdate();
    if (again)
        scheduleScan(browser);
}

void ChatIntegration::restoreAlbums(QTextBrowser* browser)
{
    QTextDocument* doc = browser->document();
    QVector<int>   markers;
    QVector<int>   hidden;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().boolProperty(kHiddenBlockProperty))
            hidden.append(block.blockNumber());
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.charFormat().hasProperty(kCollapsedLabelProperty)) {
                for (int p = f.position(); p < f.position() + f.length(); ++p)
                    markers.append(p);
            }
        }
    }
    if (markers.isEmpty() && hidden.isEmpty())
        return;
    const bool wasMutating = m_mutating;
    m_mutating             = true;
    for (int i = markers.size() - 1; i >= 0; --i)
        restoreLink(doc, markers.at(i));
    for (const int number : qAsConst(hidden))
        setBlockHidden(doc, number, false);
    m_mutating = wasMutating;
    if (View* view = viewFor(browser))
        view->positionsValid = false;
}

void ChatIntegration::indexAlbums(View& view) const
{
    view.albumsByKey.clear();
    for (auto it = view.positionsByKey.constBegin(); it != view.positionsByKey.constEnd(); ++it) {
        if (!albums::isObjectId(it.key()))
            continue;
        for (const QString& key : albumKeysOf(it.key())) {
            if (!key.isEmpty() && !view.albumsByKey.value(key).contains(it.key()))
                view.albumsByKey[key].append(it.key());
        }
    }
}

// ============================================================================================
// Drawing
// ============================================================================================

void ChatIntegration::refreshAlbumsOf(QTextBrowser* browser, const QString& key, bool pixelsOnly)
{
    Q_UNUSED(pixelsOnly); // a grid's size never depends on its items: their changes only redraw it
    View* view = viewFor(browser);
    if (!view)
        return;
    // Coalesced: GIF frames and download progress of several items give one redraw (~30 per second at
    // most, and only while the grid is on screen; one off screen is redrawn when it comes back).
    const QStringList ids = view->albumsByKey.value(key);
    for (const QString& id : ids)
        scheduleAlbumRefresh(browser, id);
}

void ChatIntegration::scheduleAlbumRefresh(QTextBrowser* browser, const QString& id)
{
    m_albumDirty[browser].insert(id);
    if (!m_albumTimer) {
        // A member QTimer: nothing of it can outlive the plugin (no functor timer pending at unload).
        m_albumTimer = new QTimer(this);
        m_albumTimer->setSingleShot(true);
        m_albumTimer->setInterval(kAlbumFrameMs);
        connect(m_albumTimer, &QTimer::timeout, this, &ChatIntegration::flushAlbumRefresh);
    }
    if (!m_albumTimer->isActive())
        m_albumTimer->start();
}

void ChatIntegration::flushAlbumRefresh()
{
    const QHash<QTextBrowser*, QSet<QString>> dirty = m_albumDirty;
    m_albumDirty.clear();
    for (auto it = dirty.constBegin(); it != dirty.constEnd(); ++it) {
        if (!m_views.contains(it.key()) || !m_views.value(it.key()).browser)
            continue;
        for (const QString& id : it.value())
            refreshPreview(it.key(), id, true);
    }
}

bool ChatIntegration::shownAlone(const QString& key) const
{
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->positionsByKey.contains(key))
            return true;
    }
    return false;
}

QImage ChatIntegration::renderAlbumFor(QTextBrowser* browser, const QString& id, QSize* logicalSize)
{
    quint32     albumId = 0;
    QStringList keys;
    if (!albums::parseObjectId(id, &albumId, &keys))
        return {};
    const PreviewStyle     style = styleFor(browser);
    const albums::Geometry geo   = albums::layout(keys.size(), style.maxWidth, style.maxHeight);
    QVector<AlbumTile>     tiles(keys.size());
    for (int i = 0; i < geo.tiles.size(); ++i) {
        const QString& key = keys.at(i);
        AlbumTile&     t   = tiles[i];
        t.entry            = key.isEmpty() ? nullptr : m_core->entry(key);
        if (!t.entry)
            continue;
        const QSize pixels = albumTileStillPixels(*t.entry, geo.tiles.at(i).size(), style.dpr);
        t.still            = m_core->still(key, pixels);
        // 2.2 spoiler: Core's predicate, as for single previews (and the reveal crossfade after a click).
        t.concealed      = m_core->isSpoilerHidden(key);
        t.concealOpacity = t.concealed ? 1.0 : spoilerCoverOpacity(key);
        const bool covered = (geo.overflow > 0 && i == geo.tiles.size() - 1) || t.concealed; // under "+N" or a cover: doesn't move
        if (!t.concealed && t.concealOpacity <= 0.0 && browser->isVisible() && spoiler::appliesTo(t.entry->kind))
            m_core->noteShownOpen(key);
        if (m_media && t.entry->kind == MediaKind::AnimatedImage && !covered) {
            // Frames are made at the tile's cover size; a single preview of the same GIF wins.
            if (!shownAlone(key))
                m_media->setFrameSize(key, pixels);
            if (m_media->mode(key) == InlineMediaController::Mode::Animated)
                t.frame = m_media->frame(key);
        }
        t.hovered = m_hoverObject == id && m_hoverTile == i && browser == m_hoverObjectIn;
        t.pressed = t.hovered && m_pressedObject == id && m_pressedTile == i && browser == m_pressBrowser && !m_seeking;
    }
    QSize  size;
    QImage img = renderAlbum(tiles, style, &size);
    // 2.2 reactions: one row for the album, keyed by its first item (ai=1), below the grid.
    if (m_reactions && !img.isNull()) {
        PreviewStyle rowStyle = style;
        rowStyle.hovered      = (m_hoverObject == id && browser == m_hoverObjectIn) || m_hoverKey == id;
        img                   = m_reactions->compose(browser, id, img, rowStyle, &size);
    }
    if (logicalSize)
        *logicalSize = size;
    return img;
}

// ============================================================================================
// Pointer
// ============================================================================================

ChatIntegration::Hit ChatIntegration::albumHit(QTextBrowser* browser, const QString& id, const QRectF& rect, const QPointF& viewportPos) const
{
    Q_UNUSED(browser);
    Hit hit;
    hit.object            = id;
    hit.rect              = rect;
    const QStringList keys = albumKeysOf(id);
    if (keys.isEmpty())
        return hit;
    const albums::Geometry g     = geometryFor(keys.size(), rect);
    const qreal            sx    = rect.width() > 0 ? g.box.width() / rect.width() : 1.0;
    const qreal            sy    = rect.height() > 0 ? g.box.height() / rect.height() : 1.0;
    const QPointF          local = viewportPos - rect.topLeft();
    const int              tile  = albums::tileAt(g, QPointF(local.x() * sx, local.y() * sy));
    if (tile < 0)
        return hit; // a gap between tiles
    hit.tile     = tile;
    hit.key      = keys.at(tile);
    hit.rect     = tileRectIn(g, tile, rect);
    hit.overflow = tile == g.tiles.size() - 1 ? g.overflow : 0;
    return hit;
}

void ChatIntegration::albumVisibleKeys(QTextBrowser* browser, const QString& id, const QRectF& rect, const QRectF& viewport, QSet<QString>* keys) const
{
    Q_UNUSED(browser);
    const QStringList      items = albumKeysOf(id);
    const albums::Geometry g     = geometryFor(items.size(), rect);
    for (int i = 0; i < g.tiles.size(); ++i) {
        if (g.overflow > 0 && i == g.tiles.size() - 1)
            break; // under "+N": no animation
        if (!items.at(i).isEmpty() && tileRectIn(g, i, rect).intersects(viewport))
            keys->insert(items.at(i));
    }
}

// Tooltips name what a tile is; built from i18n::t / arg() only (QToolTip's label outlives the plugin).
QString ChatIntegration::albumToolTip(const Hit& hit, QRect* area) const
{
    *area = hit.rect.toAlignedRect();
    if (hit.key.isEmpty())
        return i18n::t("This file hasn't arrived yet");
    if (hit.overflow > 0)
        return i18n::t("%1 more · Click to see all").arg(hit.overflow);
    if (m_core->isSpoilerHidden(hit.key))
        return spoilerToolTip(); // 2.2 spoiler: no name, size or state
    const MediaEntry* e = m_core->entry(hit.key);
    if (!e)
        return {};
    // The name comes from someone else's chat link: plain text, escaped.
    QString name = displayNameFor(e->link);
    if (e->kind == MediaKind::Video && e->link.durationMs > 0)
        name += QStringLiteral(" · ") + formatDuration(e->link.durationMs);
    else if (e->link.size)
        name += QStringLiteral(" · ") + formatSize(e->link.size);
    QString detail;
    if (e->state == MediaState::Failed) {
        detail = e->errorText.isEmpty() ? downloadErrorText(e->error) : e->errorText;
        if (isRetryableDownload(*e))
            detail += QLatin1Char(' ') + i18n::t("Click to retry.");
    } else if (e->state == MediaState::Queued || e->state == MediaState::Downloading) {
        detail = previewStatusText(*e, false);
    } else if (e->state == MediaState::Idle) {
        detail = i18n::t("Click to view");
    }
    QString text = QStringLiteral("<div style='white-space:pre'>%1</div>").arg(name.toHtmlEscaped());
    if (!detail.isEmpty())
        text += QStringLiteral("<div>%1</div>").arg(detail.toHtmlEscaped());
    return text;
}

void ChatIntegration::activateAlbumTile(const Hit& hit)
{
    const MediaEntry* e = hit.key.isEmpty() ? nullptr : m_core->entry(hit.key);
    if (!e)
        return; // not arrived yet
    // 2.2 spoiler: the first click only reveals a covered tile ("+N" still opens the viewer, which
    // has its own cover).
    if (hit.overflow == 0 && m_core->isSpoilerHidden(hit.key)) {
        revealSpoiler(hit.key);
        return;
    }
    if (e->state == MediaState::Failed) {
        // Fixable failures get another try in place; files gone from the server stay as they are.
        if (isRetryableDownload(*e))
            m_core->retry(hit.key);
        return;
    }
    // The viewer, at this item ("+N" too), with the rest of the album next to it. Opened outside the
    // event filter by a timer of ours (owned: nothing of it outlives the plugin).
    const QString key = hit.key;
    singleShotOwned(0, this, [this, key] { openViewer(key); });
}
