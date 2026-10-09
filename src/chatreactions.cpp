#include "chatreactions.h"

#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include "chatintegration.h"
#include "i18n.h"
#include "inlinemedia.h"
#include "peerhub.h"
#include "peers.h"
#include "pluginlink.h"
#include "reactionpicker.h"
#include "settings.h"
#include "ts3api.h"

namespace {

// ChatIntegration names its preview objects "tsmedia:<key>" (kScheme in chatintegration.cpp).
const char kObjectScheme[] = "tsmedia:";

constexpr int kNamesInTip   = 10;
constexpr int kKindRecheckMs = 2000;

bool isReactable(const MediaEntry* e)
{
    // Pictures, GIFs and videos (audio and voice cards follow when they exist); not file cards.
    return e && (e->kind == MediaKind::Video || isPreviewableImage(e->kind));
}

bool reactive(rx::Zone zone)
{
    return zone == rx::Zone::AddButton || zone == rx::Zone::Pill || zone == rx::Zone::AddPill || zone == rx::Zone::Row;
}

bool actionable(rx::Zone zone)
{
    return zone == rx::Zone::AddButton || zone == rx::Zone::Pill || zone == rx::Zone::AddPill;
}

// "you, Sara and Reza"; "Ali, Sara, ... and 4 others"
QString nameList(QStringList names, int total)
{
    for (QString& name : names)
        name = peers::presenceName(name);
    if (names.size() > kNamesInTip)
        names = names.mid(0, kNamesInTip);
    const int rest = qMax(0, total - names.size());
    if (rest > 0)
        return i18n::t("%1 and %2 others").arg(names.join(QStringLiteral(", "))).arg(rest);
    if (names.size() <= 1)
        return names.value(0);
    const QString last = names.takeLast();
    return i18n::t("%1 and %2").arg(names.join(QStringLiteral(", ")), last);
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

QIcon reactionIcon(int reaction, qreal dpr)
{
    QPixmap pixmap(QSize(16, 16) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    rx::drawReaction(p, QRectF(0, 0, 16, 16), reaction);
    p.end();
    return QIcon(pixmap);
}

} // namespace

ChatReactions::ChatReactions(ChatIntegration* chat, Core* core)
    : QObject(chat)
    , m_chat(chat)
    , m_core(core)
{
    if (PeerHub* hub = PeerHub::instance()) {
        connect(hub, &PeerHub::reactionsChanged, this, &ChatReactions::refreshKey);
        connect(hub, &PeerHub::reactionFailed, this, [this](const QString& key, const QString& text) {
            // At the pointer, if the chat is still in view; the log has it either way.
            QTextBrowser* chat = m_chat->visibleChatBrowser();
            ChatIntegration::View* view = chat ? m_chat->viewFor(chat) : nullptr;
            if (!view)
                return;
            m_chat->ensurePositions(*view);
            if (view->positionsByKey.contains(key))
                m_chat->showFeedback(chat->viewport(), text, true);
        });
        hub->setPresentKeys(this, [this](const QString& serverUid) { return presentKeys(serverUid); });
    }
}

ChatReactions::~ChatReactions()
{
    if (PeerHub* hub = PeerHub::instance())
        hub->setPresentKeys(nullptr, {});
    if (m_picker)
        delete m_picker.data();
}

// ---- which chat ------------------------------------------------------------------------------------

ChatReactions::Kind ChatReactions::kindOf(QTextBrowser* browser, bool reclassify)
{
    if (!browser)
        return Kind::Unknown;
    const auto known = m_kinds.constFind(browser);
    const qint64 now  = QDateTime::currentMSecsSinceEpoch();
    const bool   stale = reclassify && now - m_kindCheckedMs.value(browser) >= kKindRecheckMs;
    if (known != m_kinds.constEnd() && !stale)
        return known.value();
    // Only a chat that is shown is the current tab of its tab bar.
    if (!browser->isVisible() || !m_chat->chatTabBarFor(browser))
        return known == m_kinds.constEnd() ? Kind::Unknown : known.value();
    ChatTarget target;
    Kind       kind = Kind::Private; // a private chat whose partner can't be identified
    if (m_chat->resolveTarget(browser, &target))
        kind = target.mode == TextMessageTarget_SERVER ? Kind::Server : target.mode == TextMessageTarget_CLIENT ? Kind::Private : Kind::Channel;
    track(browser);
    // Rows drawn before the chat was known (Unknown) or for another kind are drawn again.
    const Kind before  = known == m_kinds.constEnd() ? Kind::Unknown : known.value();
    const bool changed = before != kind && (before != Kind::Unknown || kind == Kind::Server);
    m_kinds.insert(browser, kind);
    m_kindCheckedMs.insert(browser, now);
    if (changed) {
        QPointer<QTextBrowser> guard(browser);
        QTimer::singleShot(0, this, [this, guard] { // a posted event, freed with this object
            ChatIntegration::View* view = guard ? m_chat->viewFor(guard.data()) : nullptr;
            if (!view)
                return;
            m_chat->ensurePositions(*view);
            for (const QString& key : view->positionsByKey.keys())
                refresh(guard.data(), key);
        });
    }
    return kind;
}

// Everything kept per chat goes with it.
void ChatReactions::track(QTextBrowser* browser)
{
    if (m_tracked.contains(browser))
        return;
    m_tracked.insert(browser);
    connect(browser, &QObject::destroyed, this, [this, browser] {
        m_tracked.remove(browser);
        m_kinds.remove(browser);
        m_kindCheckedMs.remove(browser);
        m_geometry.remove(browser);
    });
}

bool ChatReactions::eligible(QTextBrowser* browser, const QString& key, Kind* kind)
{
    const Settings& s = Settings::instance();
    if (!s.showReactions || !s.inlinePreviews || !PeerHub::instance() || !isReactable(m_core->entry(key)))
        return false;
    *kind = kindOf(browser);
    return *kind != Kind::Server;
}

bool ChatReactions::canAdd(Kind kind) const
{
    PeerHub* hub = PeerHub::instance();
    if (!hub || (kind != Kind::Channel && kind != Kind::Private))
        return false;
    // Only a shown chat can be hovered: it belongs to the current server tab.
    const uint64 sch = ts3::currentConnection();
    return ts3::isConnected(sch) && hub->link() && !hub->link()->blocked(sch);
}

QString ChatReactions::blockedText(QTextBrowser* browser) const
{
    PeerHub* hub = PeerHub::instance();
    if (!hub)
        return i18n::t("Your reaction wasn't sent.");
    ChatTarget target;
    if (!m_chat->resolveTarget(browser, &target))
        return i18n::t("Can't tell who this private chat is with, so your reaction can't be delivered.");
    return PeerHub::errorText(hub->reactBlock(target));
}

QColor ChatReactions::baseOf(QTextBrowser* browser) const
{
    return browser ? browser->viewport()->palette().color(QPalette::Base) : QColor();
}

// ---- drawing --------------------------------------------------------------------------------------

QImage ChatReactions::compose(QTextBrowser* browser, const QString& key, const QImage& picture, const PreviewStyle& style, QSize* size)
{
    track(browser);
    Geometry& geometry = m_geometry[browser][key];
    geometry.picture   = *size;
    geometry.row       = rx::RowLayout();
    geometry.button    = false;
    Kind kind          = Kind::Unknown;
    if (picture.isNull() || !eligible(browser, key, &kind))
        return picture;

    rx::ObjectState state;
    state.dark     = style.dark;
    state.base     = baseOf(browser);
    state.font     = style.font;
    state.dpr      = style.dpr;
    state.maxWidth = style.maxWidth;
    state.hovered  = style.hovered;
    state.canAdd   = style.hovered && canAdd(kind); // only shown while hovered (spares video frames the lookups)
    state.keep     = m_keep.contains(key);
    if (m_hover.browser == browser && m_hover.key == key) {
        state.hoverZone  = m_hover.zone;
        state.hoverIndex = m_hover.index;
    }
    if (m_press.browser == browser && m_press.key == key) {
        state.pressZone  = m_press.zone;
        state.pressIndex = m_press.index;
    }
    rx::ObjectLayout layout;
    const QSize      pictureSize = *size;
    const QImage     object      = rx::composeObject(picture, pictureSize, PeerHub::instance()->view(key), state, &layout, size);
    geometry.row                 = layout.row;
    geometry.button              = layout.button;
    return object;
}

QRectF ChatReactions::pictureRect(QTextBrowser* browser, const QString& key, const QRectF& objectRect) const
{
    const auto perBrowser = m_geometry.constFind(browser);
    if (perBrowser == m_geometry.constEnd())
        return objectRect;
    const auto geometry = perBrowser->constFind(key);
    if (geometry == perBrowser->constEnd() || !geometry->picture.isValid() || geometry->row.isEmpty())
        return objectRect;
    return QRectF(objectRect.topLeft(), QSizeF(geometry->picture).boundedTo(objectRect.size()));
}

// ---- pointer ------------------------------------------------------------------------------------------

ChatReactions::Hit ChatReactions::hitAt(QTextBrowser* browser, const QPoint& pos) const
{
    ChatIntegration::View* view = m_chat->viewFor(browser);
    if (!view)
        return {};
    m_chat->ensurePositions(*view);
    QTextDocument* doc = browser->document();
    const QPointF  docPos(pos.x() + browser->horizontalScrollBar()->value(), pos.y() + browser->verticalScrollBar()->value());
    const int      hit  = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    const int      last = doc->characterCount() - 1;
    if (hit < 0)
        return {};
    for (int p : {hit, hit - 1}) {
        if (p < 0 || p + 1 > last)
            continue;
        QTextCursor c(doc);
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat format = c.charFormat();
        if (!format.isImageFormat())
            continue;
        const QString name = format.toImageFormat().name();
        if (!name.startsWith(QLatin1String(kObjectScheme)))
            continue;
        const QString key  = name.mid(static_cast<int>(sizeof(kObjectScheme)) - 1);
        const QRectF  rect = m_chat->previewRect(browser, p, QSizeF(view->formatSizes.value(key)));
        if (!rect.isValid() || !rect.contains(pos))
            continue;
        Hit result;
        result.key    = key;
        result.object = rect;
        const auto perBrowser = m_geometry.constFind(browser);
        const Geometry geometry = perBrowser == m_geometry.constEnd() ? Geometry() : perBrowser->value(key);
        if (geometry.picture.isValid())
            result.zone = rx::zoneAt(QSizeF(geometry.picture), geometry.row, geometry.button, QPointF(pos) - rect.topLeft());
        else
            result.zone.zone = rx::Zone::Picture;
        if (result.zone.zone == rx::Zone::None)
            result.zone.zone = rx::Zone::Row; // beside a narrow picture, inside the object: not the chat text
        return result;
    }
    return {};
}

void ChatReactions::refresh(QTextBrowser* browser, const QString& key)
{
    if (browser && !key.isEmpty())
        m_chat->refreshPreview(browser, key, false);
}

// After ChatIntegration has handled the same event (its hover state is what the drawing reads).
void ChatReactions::refreshLater(QTextBrowser* browser, const QString& key)
{
    QPointer<QTextBrowser> guard(browser);
    QTimer::singleShot(0, this, [this, guard, key] { // a posted event, freed with this object
        if (guard)
            refresh(guard.data(), key);
    });
}

void ChatReactions::refreshKey(const QString& key)
{
    for (auto it = m_chat->m_views.begin(); it != m_chat->m_views.end(); ++it) {
        if (it->browser)
            m_chat->refreshPreview(it->browser, key, false);
    }
}

void ChatReactions::setSpot(const Spot& spot)
{
    if (spot.same(m_hover))
        return;
    const Spot old = m_hover;
    m_hover        = spot;
    if (old.browser && !old.key.isEmpty())
        refresh(old.browser, old.key);
    if (spot.browser && !spot.key.isEmpty() && !(old.browser == spot.browser && old.key == spot.key))
        refresh(spot.browser, spot.key);
    else if (spot.browser && !spot.key.isEmpty() && old.key.isEmpty())
        refresh(spot.browser, spot.key);
}

void ChatReactions::collapseKept(const QString& stillOn)
{
    if (m_keep.isEmpty())
        return;
    const QSet<QString> kept = m_keep;
    for (const QString& key : kept) {
        if (key == stillOn)
            continue;
        m_keep.remove(key);
        refreshKey(key); // the empty row goes once the pointer has left it
    }
}

bool ChatReactions::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick && type != QEvent::MouseButtonRelease
        && type != QEvent::ContextMenu && type != QEvent::ToolTip && type != QEvent::Leave)
        return false;
    QTextBrowser* browser = m_chat->browserForViewport(watched);
    if (!browser)
        return false;

    if (type == QEvent::Leave) {
        setSpot(Spot());
        if (!m_objectKey.isEmpty())
            refreshLater(browser, m_objectKey);
        m_objectKey.clear();
        collapseKept(QString());
        return false;
    }
    if (!PeerHub::instance() || !Settings::instance().showReactions) {
        setSpot(Spot());
        return false;
    }

    QPoint pos;
    switch (type) {
    case QEvent::ContextMenu:
        pos = static_cast<QContextMenuEvent*>(event)->pos();
        break;
    case QEvent::ToolTip:
        pos = static_cast<QHelpEvent*>(event)->pos();
        break;
    default:
        pos = static_cast<QMouseEvent*>(event)->pos();
        break;
    }

    // A video seek or a press that started on a picture belongs to the picture.
    if (type == QEvent::MouseMove && (m_chat->m_seeking || (!m_chat->m_pressedKey.isEmpty() && !m_press.browser)))
        return false;

    const Hit hit = hitAt(browser, pos);
    Spot      spot;
    if (reactive(hit.zone.zone)) {
        spot.browser = browser;
        spot.key     = hit.key;
        spot.zone    = hit.zone.zone;
        spot.index   = hit.zone.index;
    }

    switch (type) {
    case QEvent::MouseMove: {
        kindOf(browser, true);
        if (m_objectKey != hit.key) {
            if (!m_objectKey.isEmpty())
                refreshLater(browser, m_objectKey); // its add pill / button goes with the hover
            m_objectKey = hit.key;
        }
        collapseKept(hit.key);
        setSpot(spot);
        if (!reactive(hit.zone.zone))
            return false; // the picture, the chat text: ChatIntegration's
        // Over the row the preview stays "hovered" (its add pill shows), outside the picture's own logic.
        if (m_chat->m_hoverKey != hit.key) {
            const QString left = m_chat->m_hoverKey;
            m_chat->m_hoverKey = hit.key;
            if (!left.isEmpty())
                refresh(browser, left);
            refresh(browser, hit.key);
        }
        if (m_chat->m_media)
            m_chat->m_media->hover(hit.key, VideoZone::None);
        m_chat->updateCursor(browser, true, pos, actionable(hit.zone.zone));
        return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        if (!reactive(hit.zone.zone))
            return false;
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && actionable(hit.zone.zone)) {
            m_press             = spot;
            m_chat->m_lastBrowser = browser;
            QToolTip::hideText();
            refresh(browser, hit.key);
        }
        return true; // no text selection or drag from the row
    }

    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) {
            return reactive(hit.zone.zone); // the context menu event follows
        }
        if (!m_press.browser)
            return false;
        const Spot pressed = m_press;
        m_press            = Spot();
        refresh(pressed.browser, pressed.key);
        if (pressed.same(spot)) {
            if (spot.zone == rx::Zone::Pill) {
                toggle(browser, spot.key, spot.index);
            } else {
                // The picker opens under what was clicked.
                const QRectF local  = hit.zone.rect.translated(hit.object.topLeft());
                const QRect  global = QRect(browser->viewport()->mapToGlobal(local.topLeft().toPoint()), local.size().toSize());
                openPicker(browser, spot.key, global);
            }
        }
        return true;
    }

    case QEvent::ContextMenu: {
        if (!reactive(hit.zone.zone))
            return false;
        m_chat->m_lastBrowser = browser;
        m_chat->showContextMenu(browser, hit.key, static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }

    case QEvent::ToolTip: {
        if (!reactive(hit.zone.zone))
            return false;
        auto*         he   = static_cast<QHelpEvent*>(event);
        const QRect   area = hit.zone.rect.translated(hit.object.topLeft()).toAlignedRect();
        QString       text;
        if (hit.zone.zone == rx::Zone::Pill)
            text = pillToolTip(hit.key, hit.zone.index);
        else if (hit.zone.zone == rx::Zone::AddButton || hit.zone.zone == rx::Zone::AddPill)
            text = addToolTip();
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

void ChatReactions::toggle(QTextBrowser* browser, const QString& key, int reaction)
{
    PeerHub* hub = PeerHub::instance();
    if (!hub || !browser)
        return;
    ChatTarget target;
    if (!m_chat->resolveTarget(browser, &target)) {
        m_chat->showFeedback(browser->viewport(), i18n::t("Can't tell who this private chat is with, so your reaction can't be delivered."), true);
        return;
    }
    const PeerHub::ReactError error = hub->toggle(key, reaction, target);
    if (error != PeerHub::ReactError::None) {
        m_chat->showFeedback(browser->viewport(), PeerHub::errorText(error), true);
        return;
    }
    // Removing the last reaction while the pointer is on the row keeps the row until it leaves.
    if (hub->view(key).isEmpty() && (m_objectKey == key || m_chat->m_hoverKey == key))
        m_keep.insert(key);
    refresh(browser, key); // at once; the other chats follow with the store's change
}

void ChatReactions::openPicker(QTextBrowser* browser, const QString& key, const QRect& anchor)
{
    PeerHub* hub = PeerHub::instance();
    if (!hub)
        return;
    if (m_picker)
        m_picker->close();
    const PreviewStyle style  = m_chat->styleFor(browser);
    auto*              picker = new ReactionPicker(style.dark, baseOf(browser), hub->view(key).ownMask());
    m_picker                  = picker;
    QPointer<QTextBrowser> guard(browser);
    connect(picker, &ReactionPicker::picked, this, [this, guard, key](int reaction) {
        if (guard)
            toggle(guard.data(), key, reaction);
    });
    picker->openAt(anchor);
}

void ChatReactions::addMenu(QMenu* menu, QTextBrowser* browser, const QString& key)
{
    PeerHub* hub  = PeerHub::instance();
    Kind     kind = Kind::Unknown;
    if (!hub || !menu || !eligible(browser, key, &kind))
        return;
    kindOf(browser, true);
    const ReactionView view    = hub->view(key);
    const QString      blocked = blockedText(browser);
    menu->addSeparator();
    QMenu* sub = menu->addMenu(i18n::t("Add &reaction"));
    sub->setToolTipsVisible(true);
    sub->setLayoutDirection(Qt::LeftToRight);
    if (!blocked.isEmpty()) {
        // Shown, but disabled with the reason (rather than missing without one).
        menu->setToolTipsVisible(true);
        sub->menuAction()->setEnabled(false);
        sub->menuAction()->setToolTip(blocked);
        return;
    }
    const qreal            dpr = browser->devicePixelRatioF();
    QPointer<QTextBrowser> guard(browser);
    for (int i = 0; i < proto::kReactionCount; ++i) {
        const int count = view.per[i].count;
        QString   text  = rx::reactionName(i);
        if (count > 0)
            text = i18n::t("%1 · %2").arg(text, rx::countText(count));
        QAction* action = sub->addAction(reactionIcon(i, dpr), text);
        action->setCheckable(true);
        action->setChecked(view.per[i].mine);
        if (count > 0)
            action->setToolTip(pillToolTip(key, i));
        connect(action, &QAction::triggered, this, [this, guard, key, i] {
            if (guard)
                toggle(guard.data(), key, i);
        });
    }
}

// ---- texts -------------------------------------------------------------------------------------------

QString ChatReactions::pillToolTip(const QString& key, int reaction) const
{
    PeerHub* hub = PeerHub::instance();
    if (!hub || reaction < 0 || reaction >= proto::kReactionCount)
        return {};
    const ReactionView::Entry entry = hub->view(key).per[reaction];
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

QString ChatReactions::addToolTip() const
{
    return i18n::t("Add reaction\nSeen by people with TS Media who are online in this chat.");
}

QStringList ChatReactions::presentKeys(const QString& serverUid) const
{
    // Most recent first: the end of each chat document is the newest.
    QStringList keys;
    for (auto it = m_chat->m_views.begin(); it != m_chat->m_views.end(); ++it) {
        if (!it->browser)
            continue;
        m_chat->ensurePositions(it.value());
        const QVector<ChatIntegration::PreviewPos>& previews = it->previews;
        for (int i = previews.size() - 1; i >= 0 && keys.size() < proto::kMaxItems; --i) {
            const QString&    key = previews.at(i).key;
            const MediaEntry* e   = m_core->entry(key);
            if (!keys.contains(key) && isReactable(e) && e->link.serverUid == serverUid)
                keys.append(key);
        }
    }
    return keys.mid(0, proto::kMaxItems);
}
