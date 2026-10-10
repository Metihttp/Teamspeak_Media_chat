#include "chatreplies.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVariantAnimation>

#include <algorithm>
#include <functional>

#include "chatemoji.h" // 2.2 emoji: its items in the chat menu
#include "chatintegration.h"
#include "emojitext.h" // 2.2 emoji: whether names and snippets show HD emoji
#include "i18n.h"
#include "medialink.h" // kMaxMessageBytes
#include "replybar.h"
#include "repliespopup.h"
#include "ts3api.h"
#include "uiutil.h"

namespace {

constexpr int   kRelayoutMs   = 120;
constexpr int   kReindexMs    = 400; // after ChatEmoji's last edit (it works through a history in steps)
constexpr int   kStyleCheckMs = 1000;
constexpr int   kMaxRendered  = 48; // reply line pictures kept per chat before those far off screen go
constexpr int   kWatchMs      = 300;
constexpr int   kScrollMs     = 260;
constexpr int   kFlashMs      = 1700;
constexpr qreal kFlashAlpha   = 0.20;

// The same chat: server tab, kind and (for a private chat) the same person.
bool sameChat(const ChatTarget& a, const ChatTarget& b)
{
    if (a.sch != b.sch || a.mode != b.mode)
        return false;
    if (a.mode != TextMessageTarget_CLIENT)
        return true;
    if (!a.clientUid.isEmpty() && !b.clientUid.isEmpty())
        return a.clientUid == b.clientUid;
    return a.clientId != 0 && a.clientId == b.clientId;
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

// The icon colour for menu items: readable on light and dark menus alike (3:1 on white and on #2b2d31).
QColor menuIconColor()
{
    return QColor(0x7d, 0x82, 0x8a);
}

// The message the jump went to, tinted for a moment (the "flash" of Discord's jump to a reply). Over the
// chat's viewport, takes no input, follows the chat while it scrolls, and goes away by itself.
class Flash : public QWidget
{
  public:
    Flash(QWidget* viewport, std::function<QRect()> area, const QColor& accent, bool animate)
        : QWidget(viewport)
        , m_area(std::move(area))
        , m_accent(accent)
    {
        setObjectName(QString::fromLatin1("tsmediaReplyFlash")); // swept at plugin shutdown like our other widgets
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        auto* fade = new QVariantAnimation(this); // a child: stops with the widget
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setDuration(kFlashMs);
        fade->setEasingCurve(animate ? QEasingCurve::InQuad : QEasingCurve::Linear);
        connect(fade, &QVariantAnimation::valueChanged, this, [this, animate](const QVariant& value) {
            // Without animations (Windows setting): a steady tint that goes at the end.
            m_opacity = animate ? value.toReal() : 1.0;
            follow();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, [this] {
            hide();
            deleteLater();
        });
        follow();
        show();
        raise();
        fade->start();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QColor   fill = m_accent;
        fill.setAlphaF(kFlashAlpha * m_opacity);
        p.fillRect(rect(), fill);
        QColor bar = m_accent;
        bar.setAlphaF(qMin(1.0, 0.9 * m_opacity));
        p.fillRect(QRect(0, 0, 3, height()), bar);
    }

  private:
    void follow()
    {
        const QRect r = m_area ? m_area() : QRect();
        if (r.isEmpty()) {
            hide();
            return;
        }
        if (r != geometry())
            setGeometry(r);
    }

    std::function<QRect()> m_area;
    QColor                 m_accent;
    qreal                  m_opacity = 1.0;
};

} // namespace

// Catches the menu TeamSpeak shows for a right-click on the chat while it is being built (the moment it
// is polished, before its size is computed), so "Reply" can go to its top. Installed on the application
// only for the length of that one context menu event.
class ReplyMenuCatcher : public QObject
{
  public:
    explicit ReplyMenuCatcher(ChatReplies* owner)
        : QObject(owner)
        , m_owner(owner)
    {
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        const QEvent::Type type = event->type();
        if (type == QEvent::Polish || type == QEvent::Show) {
            if (auto* menu = qobject_cast<QMenu*>(watched)) {
                if (!menu->objectName().startsWith(QLatin1String("tsmedia")))
                    m_owner->inject(menu);
            }
        }
        return false;
    }

  private:
    ChatReplies* m_owner;
};

ChatReplies::ChatReplies(ChatIntegration* chat, Core* core)
    : QObject(chat)
    , m_chat(chat)
    , m_core(core)
{
    // Object names of this instance: a picture left over from an earlier one (a crash) never shares one.
    m_tag = QString::number(QRandomGenerator::global()->generate(), 16);
    // A reply line off screen has this picture (a missing one would make Qt draw its "file" icon there).
    m_blank = QPixmap(1, 1);
    m_blank.fill(Qt::transparent);

    m_relayout = new QTimer(this);
    m_relayout->setSingleShot(true);
    m_relayout->setInterval(kRelayoutMs);
    connect(m_relayout, &QTimer::timeout, this, [this] {
        const QSet<QTextBrowser*> dirty = m_dirty;
        m_dirty.clear();
        for (QTextBrowser* browser : dirty) {
            if (m_views.contains(browser))
                refreshLines(browser);
        }
    });

    // Moving to a monitor with another scale or a theme switch can come without a resize or palette event.
    m_styleCheck = new QTimer(this);
    m_styleCheck->setInterval(kStyleCheckMs);
    connect(m_styleCheck, &QTimer::timeout, this, [this] {
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            QTextBrowser* browser = it.key();
            if (!it->lines.isEmpty() && browser->isVisible() && keyOf(styleOf(browser)) != it->styleKey)
                scheduleRelayout(browser); // redraws only what changed
        }
    });
    m_styleCheck->start();

    m_watch = new QTimer(this);
    m_watch->setInterval(kWatchMs);
    connect(m_watch, &QTimer::timeout, this, &ChatReplies::watchPending);

    // 2.2 emoji: HD emoji came or went in a chat; once ChatEmoji pauses, its blocks are read again (the
    // header line heights may have changed) and the reply lines laid out anew.
    m_reindex = new QTimer(this);
    m_reindex->setSingleShot(true);
    m_reindex->setInterval(kReindexMs);
    connect(m_reindex, &QTimer::timeout, this, [this] {
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            if (!it->stale)
                continue;
            it->stale = false;
            QTextBrowser* browser = it.key();
            index(it.value(), browser->document());
            refreshLines(browser);
        }
    });

    m_catcher = new ReplyMenuCatcher(this);
}

ChatReplies::~ChatReplies()
{
    disarm();
    for (const QPointer<QAction>& action : qAsConst(m_injected)) {
        if (action)
            delete action.data(); // in TeamSpeak's menu, which may still be open
    }
    m_injected.clear();
    if (m_fallback)
        delete m_fallback.data();
    if (m_popup)
        delete m_popup.data();
    if (m_flash)
        delete m_flash.data();
    if (m_scroll)
        m_scroll->stop();
    m_hasPending = false;
    if (m_bar)
        delete m_bar.data(); // leaves TeamSpeak's layout with it
    m_composeLines.clear(); // a send window still open goes right after us (ChatIntegration deletes it)

    // The chats stay with TeamSpeak: every quote line goes back where it was (one edit per chat).
    const bool wasMutating = m_chat->m_mutating;
    m_chat->m_mutating     = true;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        QTextBrowser* browser = it.key();
        if (!browser || !m_tracked.contains(browser))
            continue;
        const Anchor anchor = capture(browser);
        if (replydoc::restoreAll(browser->document()) > 0)
            restoreAnchor(browser, anchor);
        if (it->document != browser->document())
            continue; // the pictures were in a document TeamSpeak replaced
        for (auto name = it->lines.constBegin(); name != it->lines.constEnd(); ++name)
            browser->document()->addResource(QTextDocument::ImageResource, QUrl(name.key()), QPixmap()); // the pictures go too
    }
    m_chat->m_mutating = wasMutating;
}

// ============================================================================================
// Indexing
// ============================================================================================

void ChatReplies::track(QTextBrowser* browser)
{
    if (m_tracked.contains(browser))
        return;
    m_tracked.insert(browser);
    connect(browser, &QObject::destroyed, this, [this, browser] {
        m_tracked.remove(browser);
        m_views.remove(browser);
        m_dirty.remove(browser);
    });
}

ChatReplies::View* ChatReplies::viewOf(QTextBrowser* browser)
{
    if (!browser || !m_tracked.contains(browser))
        return nullptr;
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

void ChatReplies::index(View& view, QTextDocument* doc) const
{
    if (view.document != doc) {
        // Another document (TeamSpeak swapped it): its pictures have to be added to it anew.
        view.document = doc;
        view.lines.clear();
        view.rendered.clear();
    }
    view.messages = replydoc::scan(doc);
    link(view);
}

void ChatReplies::reindexFrom(View& view, QTextDocument* doc, int firstMessage) const
{
    // Blocks before the first edited message are as they were: only the rest is read again (a reply
    // that came in at the end of a long chat costs a few blocks, not the whole chat).
    if (view.document != doc || firstMessage < 0 || firstMessage >= view.messages.size()) {
        index(view, doc);
        return;
    }
    const int block = view.messages.at(firstMessage).block;
    view.messages.resize(firstMessage);
    view.messages += replydoc::scanFrom(doc, block);
    link(view);
}

void ChatReplies::link(View& view) const
{
    view.byBlock.clear();
    view.byObject.clear();
    view.repliesTo.clear();
    view.originalOf = QVector<int>(view.messages.size(), -1);
    QVector<replies::Candidate> candidates;
    candidates.reserve(view.messages.size());
    for (int i = 0; i < view.messages.size(); ++i) {
        const replydoc::Message& m = view.messages.at(i);
        view.byBlock.insert(m.block, i);
        if (m.restyled)
            view.byObject.insert(m.object, i);
        replies::Candidate c;
        c.uid     = m.uid;
        c.nick    = m.nick;
        c.minutes = m.minutes;
        c.source  = replies::snippetSource(m.text, m.mediaLabel);
        candidates.append(c);
    }
    for (int i = 0; i < view.messages.size(); ++i) {
        if (!view.messages.at(i).hasQuote)
            continue;
        const int original = replies::findOriginal(view.messages.at(i).quote, candidates, i);
        view.originalOf[i] = original;
        if (original >= 0)
            view.repliesTo[original].append(i);
    }
    view.indexed = true;
}

void ChatReplies::ensureIndex(QTextBrowser* browser)
{
    if (!browser)
        return;
    track(browser);
    View& view = m_views[browser];
    if (!view.indexed)
        index(view, browser->document());
}

void ChatReplies::afterScan(QTextBrowser* browser)
{
    if (!browser)
        return;
    track(browser);
    View&          view = m_views[browser];
    QTextDocument* doc  = browser->document();
    index(view, doc);

    // Raw quote lines that become reply lines. Their sizes first (that may lay blocks out), then all the
    // edits as one: the chat is laid out again once, not once per reply (a reloaded history with 1000
    // replies: 0.1 s instead of 1.4 s).
    struct Collapse {
        int     message;
        QString name;
        QSizeF  size;
    };
    QVector<Collapse> raw;
    Style             style;
    for (int i = 0; i < view.messages.size(); ++i) {
        const replydoc::Message& m = view.messages.at(i);
        if (!m.hasQuote || m.restyled || m.quoteStart < 0)
            continue;
        if (raw.isEmpty())
            style = styleOf(browser);
        const replyart::Header      header = headerFor(view, i);
        const replyart::HeaderStyle hs     = headerStyle(browser, style, m);
        raw.append({i, newObjectName(), QSizeF(replyart::headerSize(header, hs, style.maxWidth))});
    }
    if (!raw.isEmpty()) {
        const Anchor anchor      = capture(browser);
        const bool   wasMutating = m_chat->m_mutating;
        m_chat->m_mutating       = true;
        int         first        = -1;
        QTextCursor batch(doc);
        batch.beginEditBlock();
        // Back to front: the edits of later messages don't move earlier ones.
        for (int k = raw.size() - 1; k >= 0; --k) {
            const Collapse& c = raw.at(k);
            // Drawn once it is on screen (renderVisible); until then a blank picture, never none.
            doc->addResource(QTextDocument::ImageResource, QUrl(c.name), m_blank);
            if (replydoc::collapse(doc, view.messages.at(c.message), c.name, c.size))
                first = c.message;
        }
        batch.endEditBlock(); // TeamSpeak's view hears of it (contentsChange) while we are still "mutating"
        m_chat->m_mutating = wasMutating;
        if (first >= 0)
            reindexFrom(view, doc, first);
        restoreAnchor(browser, anchor);
        m_chat->scheduleVisibilityUpdate();
    }
    refreshLines(browser);
}

int ChatReplies::messageAt(QTextBrowser* browser, const QPoint& viewportPos)
{
    ensureIndex(browser);
    View* view = viewOf(browser);
    if (!view)
        return -1;
    QTextDocument* doc = browser->document();
    const QPointF  docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int      hit = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    if (hit < 0)
        return -1;
    const QTextBlock block = doc->findBlock(hit);
    if (!block.isValid() || !block.isVisible())
        return -1;
    const QRectF r = doc->documentLayout()->blockBoundingRect(block);
    if (docPos.y() < r.top() - 1 || docPos.y() > r.bottom() + 1)
        return -1; // below the last message
    return view->byBlock.value(block.blockNumber(), -1);
}

int ChatReplies::lineAt(QTextBrowser* browser, const QPoint& viewportPos, QRectF* rect)
{
    View* view = viewOf(browser);
    if (!view || view->lines.isEmpty())
        return -1;
    QTextDocument* doc = browser->document();
    const QPointF  docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int      hit  = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    const int      last = doc->characterCount() - 1;
    if (hit < 0)
        return -1;
    for (int p : {hit, hit - 1}) {
        if (p < 0 || p + 1 > last)
            continue;
        const QTextCharFormat format = formatAt(doc, p);
        if (!format.isImageFormat() || !format.toImageFormat().name().startsWith(replydoc::objectPrefix()))
            continue;
        const QTextBlock   block  = doc->findBlock(p);
        const QTextLayout* layout = block.layout();
        if (!layout)
            continue;
        const int       rel  = p - block.position();
        const QTextLine line = layout->lineForTextPosition(rel);
        if (!line.isValid())
            continue;
        const QRectF br = doc->documentLayout()->blockBoundingRect(block);
        const QRectF r(br.left() + line.cursorToX(rel) - browser->horizontalScrollBar()->value(), br.top() + line.y() - browser->verticalScrollBar()->value(),
                       format.toImageFormat().width(), line.height());
        if (!r.contains(viewportPos))
            continue;
        const int message = view->byBlock.value(block.blockNumber(), -1);
        if (message < 0 || !view->messages.at(message).restyled)
            continue;
        if (rect)
            *rect = r;
        return message;
    }
    return -1;
}

void ChatReplies::documentEdited(QTextBrowser* browser)
{
    View* view = viewOf(browser);
    if (!view || !view->indexed)
        return;
    view->stale = true;
    m_reindex->start(); // again: one read once the edits pause
}

// ============================================================================================
// Drawing the reply lines
// ============================================================================================

ChatReplies::Style ChatReplies::styleOf(QTextBrowser* browser) const
{
    const PreviewStyle preview = m_chat->styleFor(browser);
    Style              style;
    style.dark  = preview.dark;
    style.font  = preview.font;
    style.dpr   = preview.dpr;
    style.base  = browser->viewport()->palette().color(QPalette::Base);
    if (style.dark != (style.base.lightness() < 128))
        style.base = QColor(); // a style sheet colours the chat: the theme's default
    const qreal margin = browser->document()->documentMargin();
    style.maxWidth     = qMax(80, m_chat->previewAreaWidth(browser) - static_cast<int>(2 * margin) - 6);
    return style;
}

QString ChatReplies::keyOf(const Style& style)
{
    return QStringList{QString::number(style.dark), style.base.name(), style.font.toString(), QString::number(style.dpr), QString::number(style.maxWidth),
                       QString::number(emoji::textPicturesEnabled())} // 2.2 emoji: HD emoji turned on or off
        .join(QLatin1Char('|'));
}

replyart::Header ChatReplies::headerFor(const View& view, int message) const
{
    replyart::Header         header;
    const replydoc::Message& m        = view.messages.at(message);
    const int                original = view.originalOf.value(message, -1);
    header.nick                       = m.quote.nick;
    header.snippet                    = m.quote.snippet;
    header.media                      = m.quote.media;
    if (original >= 0) {
        // The original as this chat shows it: its current name and colour, and more of its text.
        const replydoc::Message& o = view.messages.at(original);
        header.found               = true;
        header.nick                = o.nick;
        header.nickColor           = o.nickColor;
        if (!o.text.trimmed().isEmpty()) {
            header.snippet = replies::makeSnippet(o.text, 200);
            header.media   = false;
        } else if (!o.mediaLabel.isEmpty()) {
            header.snippet = o.mediaLabel;
            header.media   = true;
        }
    }
    return header;
}

replyart::HeaderStyle ChatReplies::headerStyle(QTextBrowser* browser, const Style& style, const replydoc::Message& m) const
{
    replyart::HeaderStyle hs;
    hs.dark = style.dark;
    hs.base = style.base;
    hs.font = style.font;
    hs.dpr  = style.dpr;
    // The message line's height without the descent the line break after the picture adds (replydoc).
    QTextDocument* doc  = browser->document();
    const QFont    font = m.baseFormat.font().resolve(doc->defaultFont());
    qreal          line = m.lineHeight;
    if (line <= 0) {
        // Not laid out yet (a long history just loaded): lay this block out now.
        const QTextBlock block = doc->findBlockByNumber(m.block);
        if (block.isValid()) {
            doc->documentLayout()->blockBoundingRect(block);
            const QTextLayout* layout = block.layout();
            const int          index  = m.restyled ? 1 : 0;
            if (layout && layout->lineCount() > index)
                line = layout->lineAt(index).height();
        }
    }
    // 2.2 emoji: a message of only emoji has 48 px emoji in its header line; the reply line above it
    // stays one text line high.
    const qreal plain = QFontMetricsF(font).height();
    if (line <= 0 || line > plain * 1.6)
        line = plain;
    hs.lineHeight = qMax(8.0, line - QFontMetricsF(font).descent());
    hs.hovered    = m_hoverIn == browser && !m.object.isEmpty() && m_hoverObject == m.object;
    hs.pressed    = hs.hovered && m_pressIn == browser && m_pressObject == m.object;
    return hs;
}

QString ChatReplies::newObjectName()
{
    return replydoc::objectPrefix() + m_tag + QLatin1Char('.') + QString::number(++m_nextObject);
}

QString ChatReplies::signatureOf(const Line& line)
{
    const replyart::Header&      h  = line.header;
    const replyart::HeaderStyle& hs = line.style;
    return QStringList{h.nick, h.snippet, QString::number(h.media), QString::number(h.found), h.nickColor.name(), QString::number(line.size.width()),
                       QString::number(line.size.height()), QString::number(hs.dpr), QString::number(hs.dark), hs.base.name(), QString::number(hs.hovered),
                       QString::number(hs.pressed), hs.font.toString(), QString::number(emoji::textPicturesEnabled())}
        .join(QLatin1Char('|'));
}

void ChatReplies::refreshLines(QTextBrowser* browser, const QSet<QString>* only)
{
    View* view = viewOf(browser);
    if (!view)
        return;
    QTextDocument* doc   = browser->document();
    const Style    style = styleOf(browser);
    if (!only)
        view->styleKey = keyOf(style);
    struct Resize {
        int    message;
        QSizeF size;
    };
    QVector<Resize> resizes;
    QSet<QString>   present;
    for (int i = 0; i < view->messages.size(); ++i) {
        const replydoc::Message& m = view->messages.at(i);
        if (!m.restyled || (only && !only->contains(m.object)))
            continue;
        present.insert(m.object);
        Line line;
        line.header    = headerFor(*view, i);
        line.style     = headerStyle(browser, style, m);
        line.size      = replyart::headerSize(line.header, line.style, style.maxWidth);
        line.signature = signatureOf(line);
        if (QSizeF(line.size) != m.objectSize)
            resizes.append({i, QSizeF(line.size)});
        if (!view->lines.contains(m.object) && !view->rendered.contains(m.object))
            doc->addResource(QTextDocument::ImageResource, QUrl(m.object), m_blank); // one we see for the first time (another document)
        view->lines.insert(m.object, line);
    }
    if (!only) {
        // Reply lines that went (TeamSpeak cleared or reloaded the chat): their pictures go with them.
        for (auto it = view->lines.begin(); it != view->lines.end();) {
            if (present.contains(it.key())) {
                ++it;
                continue;
            }
            doc->addResource(QTextDocument::ImageResource, QUrl(it.key()), QPixmap());
            view->rendered.remove(it.key());
            it = view->lines.erase(it);
        }
    }
    if (!resizes.isEmpty()) {
        // A new width or font: every line that changed size, in one edit (laid out again once).
        const Anchor anchor      = capture(browser);
        const bool   wasMutating = m_chat->m_mutating;
        m_chat->m_mutating       = true;
        QTextCursor batch(doc);
        batch.beginEditBlock();
        for (const Resize& r : qAsConst(resizes)) {
            replydoc::Message& m = view->messages[r.message];
            // By its block: HD emoji may have moved positions since the index was read (never blocks).
            const QTextBlock block = doc->findBlockByNumber(m.block);
            if (!block.isValid())
                continue;
            m.position = block.position();
            replydoc::resizeObject(doc, m.position, r.size);
            m.objectSize = r.size; // a format change: no position moved
        }
        batch.endEditBlock();
        m_chat->m_mutating = wasMutating;
        restoreAnchor(browser, anchor);
    }
    renderVisible(browser, *view);
    if (!only)
        evict(browser, *view);
    browser->viewport()->update();
}

// The numbers of the first and the last block the viewport shows (margin: that many viewport heights more
// above and below). Block numbers, not positions: ChatEmoji's edits move positions inside blocks without
// telling ChatIntegration, but never blocks.
QPair<int, int> ChatReplies::blocksShown(QTextBrowser* browser, qreal margin) const
{
    QTextDocument*               doc    = browser->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const qreal                  y      = browser->verticalScrollBar()->value();
    const qreal                  height = browser->viewport()->height();
    const int                    from   = layout->hitTest(QPointF(0, qMax(0.0, y - margin * height)), Qt::FuzzyHit);
    const int                    to     = layout->hitTest(QPointF(browser->viewport()->width(), y + height + margin * height), Qt::FuzzyHit);
    return {from < 0 ? 0 : doc->findBlock(from).blockNumber(), to < 0 ? doc->blockCount() - 1 : doc->findBlock(to).blockNumber()};
}

// Called right before the chat paints (and after every refresh): the reply lines on screen get their
// pictures, the others keep a blank one. A few lines are drawn at a time, so this stays cheap.
void ChatReplies::renderVisible(QTextBrowser* browser, View& view)
{
    if (view.lines.isEmpty() || view.messages.isEmpty() || view.document != browser->document())
        return;
    QTextDocument*        doc   = browser->document();
    const QPair<int, int> shown = blocksShown(browser, 0.0);
    auto it = std::lower_bound(view.messages.cbegin(), view.messages.cend(), shown.first, [](const replydoc::Message& m, int block) { return m.block < block; });
    bool drew = false;
    for (; it != view.messages.cend() && it->block <= shown.second; ++it) {
        if (!it->restyled)
            continue;
        const auto line = view.lines.constFind(it->object);
        if (line == view.lines.cend())
            continue;
        const auto now = view.rendered.constFind(it->object);
        if (now != view.rendered.cend() && *now == line->signature)
            continue;
        doc->addResource(QTextDocument::ImageResource, QUrl(it->object), QPixmap::fromImage(replyart::renderHeader(line->header, line->style, line->size)));
        view.rendered.insert(it->object, line->signature);
        drew = true;
    }
    if (drew && view.rendered.size() > kMaxRendered)
        evict(browser, view);
}

// Pictures of reply lines more than a screen away go back to the blank one (memory stays bounded however
// long the chat and however far it was scrolled).
void ChatReplies::evict(QTextBrowser* browser, View& view)
{
    if (view.rendered.isEmpty() || view.document != browser->document())
        return;
    QTextDocument*        doc  = browser->document();
    const QPair<int, int> keep = blocksShown(browser, 1.0);
    for (auto it = view.rendered.begin(); it != view.rendered.end();) {
        const int  message = view.byObject.value(it.key(), -1);
        const bool near    = message >= 0 && view.messages.at(message).block >= keep.first && view.messages.at(message).block <= keep.second;
        if (near) {
            ++it;
            continue;
        }
        doc->addResource(QTextDocument::ImageResource, QUrl(it.key()), m_blank);
        it = view.rendered.erase(it);
    }
}

void ChatReplies::scheduleRelayout(QTextBrowser* browser)
{
    m_dirty.insert(browser);
    if (!m_relayout->isActive())
        m_relayout->start();
}

void ChatReplies::setHover(QTextBrowser* browser, const QString& object)
{
    if (browser == m_hoverIn && object == m_hoverObject)
        return;
    const QPointer<QTextBrowser> left       = m_hoverIn;
    const QString                leftObject = m_hoverObject;
    m_hoverIn                               = object.isEmpty() ? nullptr : browser;
    m_hoverObject                           = object;
    // Only the two lines whose look changed (a long chat has hundreds of reply lines).
    if (left && !leftObject.isEmpty()) {
        const QSet<QString> one{leftObject};
        refreshLines(left, &one);
    }
    if (browser && !object.isEmpty()) {
        const QSet<QString> one{object};
        refreshLines(browser, &one);
    }
}

ChatReplies::Anchor ChatReplies::capture(QTextBrowser* browser) const
{
    Anchor         anchor;
    QTextDocument* doc = browser->document();
    anchor.bottom      = ChatIntegration::atBottom(browser);
    const int y        = browser->verticalScrollBar()->value();
    const int hit      = doc->documentLayout()->hitTest(QPointF(doc->documentMargin() + 1, y + 1), Qt::FuzzyHit);
    if (hit >= 0) {
        const QTextBlock block = doc->findBlock(hit);
        anchor.block           = block.blockNumber();
        anchor.offset          = y - doc->documentLayout()->blockBoundingRect(block).top();
    }
    return anchor;
}

// What the user was reading stays where it was (S0: TeamSpeak doesn't keep the chat pinned by itself).
void ChatReplies::restoreAnchor(QTextBrowser* browser, const Anchor& anchor) const
{
    QTextDocument* doc = browser->document();
    doc->documentLayout()->documentSize(); // lays out the change, so the range is current
    QScrollBar* bar = browser->verticalScrollBar();
    if (anchor.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    const QTextBlock block = doc->findBlockByNumber(anchor.block);
    if (anchor.block >= 0 && block.isValid())
        bar->setValue(qRound(doc->documentLayout()->blockBoundingRect(block).top() + anchor.offset));
}

// ============================================================================================
// Events
// ============================================================================================

bool ChatReplies::filterEvent(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::KeyPress || type == QEvent::ShortcutOverride) {
        if (watched->inherits("ChatLineEdit"))
            return handleKey(static_cast<QWidget*>(watched), static_cast<QKeyEvent*>(event), type == QEvent::ShortcutOverride);
        return false;
    }
    if ((type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show) && m_barOverlay && watched == m_barInput.data()) {
        placeOverlayBar();
        return false;
    }
    switch (type) { // the viewport events below; everything else passes untouched
    case QEvent::Paint:
    case QEvent::Resize:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::Show:
    case QEvent::MouseMove:
    case QEvent::Leave:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::ContextMenu:
    case QEvent::ToolTip:
        break;
    default:
        return false;
    }
    QTextBrowser* browser = m_chat->browserForViewport(watched);
    if (!browser)
        return false;

    switch (type) {
    case QEvent::Paint:
        // Before the chat paints: the reply lines it is about to show get their pictures (scrolling,
        // a resize, a new message: whatever brought them on screen). Cheap when nothing is missing.
        if (View* view = viewOf(browser))
            renderVisible(browser, *view);
        return false;

    case QEvent::Resize:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::Show:
        scheduleRelayout(browser);
        return false;

    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_chat->m_seeking || (me->buttons() != Qt::NoButton && m_pressObject.isEmpty()))
            return false; // a seek or a text selection going on
        QRectF    rect;
        const int message = lineAt(browser, me->pos(), &rect);
        if (message < 0) {
            if (!m_hoverObject.isEmpty())
                setHover(nullptr, QString());
            return false;
        }
        View*      view  = viewOf(browser);
        const bool found = view->originalOf.value(message, -1) >= 0;
        if (!m_chat->m_hoverKey.isEmpty() && m_chat->m_media)
            m_chat->updateHoverAt(browser, me->localPos(), true); // the preview it came from loses its hover look
        setHover(browser, view->messages.at(message).object);
        m_chat->updateCursor(browser, true, me->pos(), found);
        return true;
    }

    case QEvent::Leave:
        if (!m_hoverObject.isEmpty())
            setHover(nullptr, QString());
        return false;

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        const int message = lineAt(browser, me->pos(), nullptr);
        if (message < 0)
            return false;
        m_pressIn     = browser;
        m_pressObject = viewOf(browser)->messages.at(message).object;
        const QSet<QString> one{m_pressObject};
        refreshLines(browser, &one);
        return true; // no text selection from a reply line
    }

    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || m_pressObject.isEmpty())
            return false;
        const QString                pressed   = m_pressObject;
        const QPointer<QTextBrowser> pressedIn = m_pressIn;
        const bool                   same      = m_pressIn == browser;
        m_pressObject.clear();
        m_pressIn = nullptr;
        if (pressedIn) {
            const QSet<QString> one{pressed};
            refreshLines(pressedIn, &one);
        }
        const int message = lineAt(browser, me->pos(), nullptr);
        if (same && message >= 0 && viewOf(browser)->messages.at(message).object == pressed)
            activateLine(browser, message);
        return true;
    }

    case QEvent::ContextMenu: {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        prepareMenu(browser, ce->pos(), ce->globalPos());
        return false; // TeamSpeak's menu (or the preview's) opens; "Reply" goes into it
    }

    case QEvent::ToolTip: {
        auto*     he      = static_cast<QHelpEvent*>(event);
        QRectF    rect;
        const int message = lineAt(browser, he->pos(), &rect);
        if (message < 0)
            return false;
        const View*   view     = viewOf(browser);
        const int     original = view->originalOf.value(message, -1);
        QString       html;
        if (original >= 0) {
            const replydoc::Message& o    = view->messages.at(original);
            const QString            text = o.text.trimmed().isEmpty() ? o.mediaLabel : replies::makeSnippet(o.text, 280);
            html = QString::fromLatin1("<div style='white-space:pre'>%1</div>").arg(i18n::t("Click to show the original message").toHtmlEscaped());
            if (!text.isEmpty())
                html += QString::fromLatin1("<div>%1</div>").arg((o.nick + QStringLiteral(": ") + text).toHtmlEscaped());
        } else {
            html = QString::fromLatin1("<div>%1</div>").arg(i18n::t("The original message isn't in this chat any more.").toHtmlEscaped());
        }
        QToolTip::showText(he->globalPos(), html, browser->viewport(), rect.toAlignedRect());
        return true;
    }

    default:
        break;
    }
    return false;
}

// ============================================================================================
// Menus
// ============================================================================================

void ChatReplies::prepareMenu(QTextBrowser* browser, const QPoint& pos, const QPoint& globalPos)
{
    m_menu             = MenuContext();
    const int  message = messageAt(browser, pos);
    const bool onEmoji = m_chat->m_emoji && m_chat->m_emoji->emojiUnder(browser, pos) >= 0; // 2.2 emoji
    if (message < 0 && !onEmoji)
        return; // not on a message (blank space, a status line, one of our own prints) nor an HD emoji
    m_menu.browser   = browser;
    m_menu.block     = message >= 0 ? viewOf(browser)->messages.at(message).block : -1;
    m_menu.pos       = pos;
    m_menu.globalPos = globalPos;
    m_menu.onEmoji   = onEmoji;
    arm();
    // Posted twice: the check runs once TeamSpeak's handler returned and once whatever that handler
    // posted ran too (a menu opened from a queued call is caught as well, instead of ours showing next to
    // it). With exec() both run inside its menu's own event loop, after the menu was caught. Only when no
    // menu showed at all does ours. A newer right-click makes these checks void.
    const quint32 serial = ++m_menuSerial;
    QTimer::singleShot(0, this, [this, serial] {
        if (serial != m_menuSerial)
            return;
        QTimer::singleShot(0, this, [this, serial] {
            if (serial != m_menuSerial)
                return;
            disarm();
            if (!m_menu.handled && m_menu.browser && (m_menu.block >= 0 || m_menu.onEmoji) && !QApplication::activePopupWidget())
                showFallbackMenu();
        });
    });
}

void ChatReplies::arm()
{
    if (m_armed)
        return;
    m_armed = true;
    qApp->installEventFilter(m_catcher);
}

void ChatReplies::disarm()
{
    if (!m_armed)
        return;
    m_armed = false;
    qApp->removeEventFilter(m_catcher);
}

void ChatReplies::inject(QMenu* menu)
{
    disarm();
    if (m_menu.handled || !m_menu.browser || (m_menu.block < 0 && !m_menu.onEmoji))
        return;
    m_menu.handled              = true;
    const QList<QAction*> ours  = actionsFor(menu, true);
    if (ours.isEmpty())
        return;
    // Caught at Show: a menu TeamSpeak keeps and shows again, placed for its old size already.
    const bool shown = menu->isVisible();
    QAction*   first = menu->actions().value(0, nullptr);
    menu->insertActions(first, ours);
    for (QAction* action : ours)
        m_injected.append(action);
    if (first) {
        auto* separator = new QAction(menu);
        separator->setSeparator(true);
        menu->insertAction(first, separator);
        m_injected.append(separator);
    }
    // A menu TeamSpeak keeps for the next time gets them anew then. Unique: such a menu (the client menu
    // a nickname's right-click may show) collects no connection per right-click.
    connect(menu, &QMenu::aboutToHide, this, &ChatReplies::dropInjected, Qt::UniqueConnection);
    if (shown) {
        // A visible menu grows downwards by our items (QMenu resizes itself): near the bottom of the
        // screen it would reach past it. Moved up as far as needed.
        const QScreen* screen = QGuiApplication::screenAt(menu->geometry().center());
        if (!screen)
            screen = menu->screen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            QRect       g    = menu->geometry();
            if (g.bottom() > area.bottom())
                g.moveBottom(area.bottom());
            if (g.top() < area.top())
                g.moveTop(area.top());
            if (g.topLeft() != menu->geometry().topLeft())
                menu->move(g.topLeft());
        }
    }
}

void ChatReplies::dropInjected()
{
    // Later, not now: an item's trigger comes after its menu hid.
    for (const QPointer<QAction>& action : qAsConst(m_injected)) {
        if (action)
            action->deleteLater();
    }
    m_injected.clear();
}

QList<QAction*> ChatReplies::actionsFor(QMenu* menu, bool chatMenu)
{
    QList<QAction*> actions;
    QTextBrowser*   browser = m_menu.browser;
    if (!menu || !browser)
        return actions;
    const int                    block     = m_menu.block;
    const QPoint                 globalPos = m_menu.globalPos;
    View*                        view      = viewOf(browser);
    const int                    idx       = view && block >= 0 ? view->byBlock.value(block, -1) : -1;
    ChatEmoji*                   emoji     = chatMenu ? m_chat->m_emoji : nullptr;
    const qreal                  dpr       = menu->devicePixelRatioF();
    const QPointer<QTextBrowser> guard(browser);

    // 2.2 emoji: the HD emoji right-clicked comes first: its name, "Copy emoji", "Use in the chat input".
    if (emoji && m_menu.onEmoji) {
        const QList<QAction*> own = emoji->emojiActions(menu, browser, m_menu.pos);
        if (!own.isEmpty()) {
            actions << own;
            auto* separator = new QAction(menu);
            separator->setSeparator(true);
            actions << separator;
        }
    }
    if (idx < 0) {
        // Not a message (an HD emoji in a line of TeamSpeak's own): "Copy text" copies that line.
        if (emoji) {
            QTextDocument*   doc  = browser->document();
            const QPointF    at(m_menu.pos.x() + browser->horizontalScrollBar()->value(), m_menu.pos.y() + browser->verticalScrollBar()->value());
            const int        hit  = doc->documentLayout()->hitTest(at, Qt::FuzzyHit);
            const QTextBlock line = doc->findBlock(qMax(0, hit));
            if (QAction* copy = emoji->copyTextAction(menu, browser, hit, line.position(), line.position() + line.length() - 1))
                actions << copy;
        }
        if (!actions.isEmpty() && actions.last()->isSeparator())
            delete actions.takeLast();
        return actions;
    }

    auto* reply = new QAction(replyart::glyphIcon(replyart::Glyph::Reply, menuIconColor(), dpr), i18n::t("&Reply"), menu);
    ChatTarget target;
    if (!m_chat->resolveTarget(browser, &target)) {
        // Shown, but disabled with the reason (rather than missing without one).
        reply->setEnabled(false);
        reply->setToolTip(i18n::t("Can't tell who this private chat is with, so you can't reply here."));
        menu->setToolTipsVisible(true);
    }
    connect(reply, &QAction::triggered, this, [this, guard, block] {
        if (guard)
            startReply(guard.data(), block, false);
    });
    actions << reply;

    const int count = view->repliesTo.value(idx).size();
    if (count > 0) {
        const QString text = count == 1 ? i18n::t("View 1 reply") : i18n::t("View %1 replies").arg(count);
        auto*         list = new QAction(replyart::glyphIcon(replyart::Glyph::Replies, menuIconColor(), dpr), text, menu);
        connect(list, &QAction::triggered, this, [this, guard, block, globalPos] {
            if (guard)
                showReplies(guard.data(), block, globalPos);
        });
        actions << list;
    }

    // 2.2 emoji: "Copy text": what the person wrote (no header, no quote line), with emoji and smileys as
    // text; the selection instead when the click is in it.
    if (emoji) {
        QTextDocument*          doc   = browser->document();
        const QTextBlock        tb    = doc->findBlockByNumber(block);
        const replydoc::Message fresh = replydoc::parseBlock(tb); // positions as they are now
        const QPointF           at(m_menu.pos.x() + browser->horizontalScrollBar()->value(), m_menu.pos.y() + browser->verticalScrollBar()->value());
        if (tb.isValid() && fresh.block >= 0) {
            if (QAction* copy = emoji->copyTextAction(menu, browser, doc->documentLayout()->hitTest(at, Qt::FuzzyHit), fresh.textStart, tb.position() + tb.length() - 1))
                actions << copy;
        }
    }
    return actions;
}

void ChatReplies::addPreviewMenu(QMenu* menu, QTextBrowser* browser)
{
    if (!menu || !browser || m_menu.browser != browser || m_menu.block < 0 || m_menu.handled)
        return;
    m_menu.handled             = true;
    const QList<QAction*> ours = actionsFor(menu, false);
    if (ours.isEmpty())
        return;
    QAction* first = menu->actions().value(0, nullptr);
    menu->insertActions(first, ours);
    if (first)
        menu->insertSeparator(first);
}

void ChatReplies::showFallbackMenu()
{
    QTextBrowser* browser = m_menu.browser;
    if (!browser)
        return;
    if (m_fallback)
        m_fallback->close();
    m_menu.handled = true;
    auto* menu     = new QMenu(browser);
    menu->setObjectName(QString::fromLatin1("tsmediaReplyMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    const QList<QAction*> ours = actionsFor(menu, true);
    if (ours.isEmpty()) {
        delete menu;
        return;
    }
    menu->addActions(ours);
    m_fallback = menu;
    menu->popup(m_menu.globalPos);
}

void ChatReplies::showReplies(QTextBrowser* browser, int block, const QPoint& globalPos)
{
    ensureIndex(browser);
    View*     view = viewOf(browser);
    const int idx  = view ? view->byBlock.value(block, -1) : -1;
    if (idx < 0)
        return;
    const QVector<int> list = view->repliesTo.value(idx);
    if (list.isEmpty())
        return;
    QVector<ReplyRow> rows;
    QVector<int>      blocks;
    for (const int r : list) {
        const replydoc::Message& m = view->messages.at(r);
        ReplyRow                 row;
        row.nick      = m.nick;
        row.nickColor = m.nickColor;
        row.time      = m.minutes >= 0 ? QStringLiteral("%1:%2").arg(m.minutes / 60, 2, 10, QLatin1Char('0')).arg(m.minutes % 60, 2, 10, QLatin1Char('0')) : QString();
        row.media     = m.text.trimmed().isEmpty() && !m.mediaLabel.isEmpty();
        row.text      = row.media ? m.mediaLabel : replies::makeSnippet(m.text, 400);
        rows.append(row);
        blocks.append(m.block);
    }
    const replydoc::Message& original = view->messages.at(idx);
    const QString title = rows.size() == 1 ? i18n::t("1 reply to %1").arg(original.nick) : i18n::t("%1 replies to %2").arg(rows.size()).arg(original.nick);
    if (m_popup)
        m_popup->close();
    const Style style = styleOf(browser);
    auto*       popup = new RepliesPopup(style.dark, style.base.isValid() ? style.base : browser->viewport()->palette().color(QPalette::Base), style.font, title, rows);
    m_popup           = popup;
    const QPointer<QTextBrowser> guard(browser);
    connect(popup, &RepliesPopup::activated, this, [this, guard, blocks](int row) {
        if (guard && row >= 0 && row < blocks.size())
            jumpTo(guard.data(), blocks.at(row));
    });
    popup->openAt(globalPos);
}

// ============================================================================================
// Reply mode
// ============================================================================================

QWidget* ChatReplies::inputFor(QTextBrowser* browser) const
{
    QWidget* fallback = nullptr;
    for (const QPointer<QWidget>& input : m_chat->m_inputs) {
        if (!input || !input->isVisible())
            continue;
        if (browser && m_chat->chatBrowserFor(input) == browser)
            return input;
        if (!fallback)
            fallback = input;
    }
    return browser ? nullptr : fallback;
}

void ChatReplies::startReply(QTextBrowser* browser, int block, bool show)
{
    ensureIndex(browser);
    View*     view = viewOf(browser);
    const int idx  = view ? view->byBlock.value(block, -1) : -1;
    if (idx < 0)
        return;
    const replydoc::Message& m = view->messages.at(idx);
    ChatTarget               target;
    if (!m_chat->resolveTarget(browser, &target)) {
        tip(browser->viewport(), i18n::t("Can't tell who this private chat is with, so you can't reply here."), true);
        return;
    }
    if (!ts3::isConnected(target.sch)) {
        tip(browser->viewport(), notConnectedText(), true);
        return;
    }
    Pending pending;
    pending.browser             = browser;
    pending.target              = target;
    pending.block               = m.block;
    pending.nickColor           = m.nickColor;
    pending.original.nick       = m.nick;
    pending.original.uid        = m.uid;
    pending.original.clientId   = m.uid.isEmpty() ? 0 : ts3::clientIdByUid(target.sch, m.uid); // 0: not on the server now
    pending.original.minutes    = m.minutes;
    pending.original.text       = m.text;
    pending.original.mediaLabel = m.mediaLabel;
    pending.media               = m.text.trimmed().isEmpty() && !m.mediaLabel.isEmpty();
    pending.snippet             = pending.media ? m.mediaLabel : replies::makeSnippet(m.text, 160);
    m_pending                   = pending;
    m_hasPending                = true;
    updateComposeLines();

    QWidget* input = inputFor(browser);
    if (input) {
        showBar(input);
        input->window()->activateWindow();
        input->setFocus(Qt::OtherFocusReason);
    }
    m_watch->start();
    if (show)
        jumpTo(browser, m.block);
}

void ChatReplies::cancelReply()
{
    const bool had = m_hasPending;
    m_hasPending   = false;
    m_pending      = Pending();
    m_watch->stop();
    updateComposeLines();
    if (had)
        hideBar();
}

void ChatReplies::watchPending()
{
    if (!m_hasPending) {
        m_watch->stop();
        return;
    }
    if (!m_pending.browser || !ts3::isConnected(m_pending.target.sch)) {
        cancelReply();
        return;
    }
    // The bar shows while the chat it belongs to is the one above the input; another chat tab hides it
    // (and Enter sends normally there) until that chat is shown again.
    QWidget*   input = inputFor(m_pending.browser);
    const bool shown = input && m_pending.browser->isVisible();
    if (shown && (!m_bar || m_bar->isHidden() || m_barInput != input))
        showBar(input);
    else if (!shown && m_bar && !m_bar->isHidden())
        hideBar();
    else if (shown && m_bar)
        applyBarTheme(input); // TeamSpeak's theme switched meanwhile (a no-op otherwise)
}

bool ChatReplies::barShownFor(QWidget* input) const
{
    return m_hasPending && m_bar && !m_bar->isHidden() && m_barInput == input && m_pending.browser && m_pending.browser->isVisible();
}

void ChatReplies::attachBar(QWidget* input)
{
    if (!m_bar) {
        m_bar = new ReplyBar;
        connect(m_bar, &ReplyBar::canceled, this, [this] {
            QWidget* input = m_barInput;
            cancelReply();
            if (input)
                input->setFocus(Qt::OtherFocusReason);
        });
        connect(m_bar, &ReplyBar::jumpRequested, this, &ChatReplies::jumpToPending);
    }
    if (m_barInput == input && m_bar->parentWidget())
        return;
    m_barInput   = input;
    m_barOverlay = false;
    // Into the vertical layout that holds the input (directly or through a row around it), right above it.
    QWidget* holder = input->parentWidget();
    QLayout* root   = holder ? holder->layout() : nullptr;
    struct Step {
        QLayout* layout;
        int      index;
    };
    QVector<Step>                          path;
    std::function<bool(QLayout*)> find = [&](QLayout* layout) -> bool {
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem* item = layout->itemAt(i);
            if (item->widget() == input) {
                path.append({layout, i});
                return true;
            }
            if (QLayout* sub = item->layout()) {
                path.append({layout, i});
                if (find(sub))
                    return true;
                path.removeLast();
            }
        }
        return false;
    };
    if (root && find(root)) {
        for (int k = path.size() - 1; k >= 0; --k) {
            auto* box = qobject_cast<QBoxLayout*>(path.at(k).layout);
            if (!box || (box->direction() != QBoxLayout::TopToBottom && box->direction() != QBoxLayout::BottomToTop))
                continue;
            box->insertWidget(box->direction() == QBoxLayout::TopToBottom ? path.at(k).index : path.at(k).index + 1, m_bar);
            return;
        }
    }
    // No such layout: the bar floats over the chat, right above the input.
    m_bar->setParent(holder ? holder : input->window());
    m_barOverlay = true;
    placeOverlayBar();
}

void ChatReplies::placeOverlayBar()
{
    if (!m_bar || !m_barInput || !m_barOverlay)
        return;
    QWidget*    input  = m_barInput;
    const int   height = m_bar->sizeHint().height();
    const QPoint topLeft = input->mapTo(m_bar->parentWidget(), QPoint(0, 0));
    m_bar->setGeometry(topLeft.x(), topLeft.y() - height, input->width(), height);
    m_bar->raise();
}

void ChatReplies::applyBarTheme(QWidget* input)
{
    if (!m_bar || !input)
        return;
    const Style style = m_pending.browser ? styleOf(m_pending.browser) : Style();
    QColor      base  = input->palette().color(QPalette::Base);
    if (style.dark != (base.lightness() < 128))
        base = style.base;
    m_bar->setTheme(style.dark, base, style.font);
}

void ChatReplies::showBar(QWidget* input)
{
    if (!input || !m_hasPending)
        return;
    attachBar(input);
    applyBarTheme(input);
    m_bar->setReply(m_pending.original.nick, m_pending.nickColor, m_pending.snippet, m_pending.media);
    QTextBrowser* chat   = m_pending.browser;
    const bool    bottom = chat && ChatIntegration::atBottom(chat);
    m_bar->show();
    if (m_barOverlay) {
        placeOverlayBar();
    } else if (QWidget* holder = m_bar->parentWidget()) {
        if (holder->layout())
            holder->layout()->activate();
    }
    // The chat got a bar's height shorter: one that showed its end still does.
    if (bottom && chat) {
        chat->verticalScrollBar()->setValue(chat->verticalScrollBar()->maximum());
        const QPointer<QTextBrowser> guard(chat);
        QTimer::singleShot(0, this, [guard] {
            if (guard)
                guard->verticalScrollBar()->setValue(guard->verticalScrollBar()->maximum());
        });
    }
    QToolTip::hideText();
}

void ChatReplies::hideBar()
{
    if (!m_bar || m_bar->isHidden())
        return;
    // The chat above gets the height back; keep it at its end if it was there.
    QTextBrowser* chat   = m_chat->visibleChatBrowser();
    const bool    bottom = chat && ChatIntegration::atBottom(chat);
    m_bar->hide();
    if (bottom) {
        const QPointer<QTextBrowser> guard(chat);
        QTimer::singleShot(0, this, [guard] {
            if (guard)
                guard->verticalScrollBar()->setValue(guard->verticalScrollBar()->maximum());
        });
    }
}

bool ChatReplies::handleKey(QWidget* input, QKeyEvent* event, bool shortcutOverride)
{
    const int                   key    = event->key();
    const Qt::KeyboardModifiers mods   = event->modifiers() & ~Qt::KeypadModifier;
    const bool                  enter  = (key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::NoModifier;
    const bool                  escape = key == Qt::Key_Escape && mods == Qt::NoModifier;
    const bool                  older  = key == Qt::Key_Up && mods == Qt::AltModifier;
    const bool                  newer  = key == Qt::Key_Down && mods == Qt::AltModifier;
    const bool                  active = barShownFor(input);
    if (shortcutOverride) {
        // Ours, not a shortcut of TeamSpeak's: the key press then comes to the input (and to us).
        if (((enter || escape) && active) || older || (newer && active)) {
            event->accept();
            return true;
        }
        return false;
    }
    if (enter && active) {
        if (event->isAutoRepeat())
            return true; // a held Enter sends once
        return trySend(input);
    }
    if (escape && active) {
        cancelReply();
        return true;
    }
    if (older || (newer && active)) {
        step(input, older ? -1 : 1);
        return true;
    }
    return false;
}

bool ChatReplies::trySend(QWidget* input)
{
    auto* edit = qobject_cast<QTextEdit*>(input);
    if (!edit || !m_hasPending)
        return false;
    const QString typed   = edit->toPlainText();
    const QString trimmed = typed.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('/')))
        return false; // nothing to send, or one of TeamSpeak's commands: TeamSpeak's
    ChatTarget now;
    if (!m_chat->resolveTarget(edit, &now)) {
        tip(edit, i18n::t("Can't tell who this private chat is with, so your reply wasn't sent."), true, false);
        return true;
    }
    if (!sameChat(now, m_pending.target)) {
        cancelReply(); // another chat now: what was typed goes there as a normal message
        return false;
    }
    if (!ts3::isConnected(now.sch)) {
        tip(edit, notConnectedText(), true, false);
        return true;
    }
    const QString message = replies::composeReply(m_pending.original, typed, kMaxMessageBytes);
    if (message.isEmpty()) {
        tip(edit, i18n::t("This reply is too long to send. Shorten it, or press Esc to send it as a normal message."), true, false);
        return true;
    }
    const unsigned error = send(now, message);
    if (error != ERROR_ok) {
        tip(edit, i18n::t("Your reply wasn't sent: %1").arg(ts3::errorText(error)), true, false);
        return true;
    }
    edit->clear();
    cancelReply();
    return true;
}

// Like a message typed in the chat: no return code of ours, so TeamSpeak reports a refusal (flooding,
// no permission) itself, as it does for anything typed.
unsigned ChatReplies::send(const ChatTarget& target, const QString& message) const
{
    const QByteArray utf8 = message.toUtf8();
    switch (target.mode) {
    case TextMessageTarget_SERVER:
        return ts3::funcs.requestSendServerTextMsg ? ts3::funcs.requestSendServerTextMsg(target.sch, utf8.constData(), nullptr) : ERROR_undefined;
    case TextMessageTarget_CLIENT: {
        // Client ids are reused: the message follows the person (resolveTarget found them by the tab).
        anyID client = target.clientId;
        if (!target.clientUid.isEmpty() && ts3::clientUid(target.sch, client) != target.clientUid)
            client = ts3::clientIdByUid(target.sch, target.clientUid);
        if (!client || !ts3::funcs.requestSendPrivateTextMsg)
            return ERROR_client_invalid_id;
        return ts3::funcs.requestSendPrivateTextMsg(target.sch, utf8.constData(), client, nullptr);
    }
    default:
        return ts3::funcs.requestSendChannelTextMsg ? ts3::funcs.requestSendChannelTextMsg(target.sch, utf8.constData(), ts3::ownChannel(target.sch), nullptr)
                                                    : ERROR_undefined;
    }
}

// Alt+Up: reply to the newest message, or to the one before the message being replied to; Alt+Down: the
// next one, and past the newest the reply is canceled.
void ChatReplies::step(QWidget* input, int direction)
{
    QTextBrowser* browser = m_chat->chatBrowserFor(input);
    if (!browser || !browser->isVisible())
        return;
    ensureIndex(browser);
    View* view = viewOf(browser);
    if (!view || view->messages.isEmpty())
        return;
    int current = -1;
    if (m_hasPending && m_pending.browser == browser)
        current = view->byBlock.value(m_pending.block, -1);
    int next = -1;
    if (current < 0) {
        if (direction > 0)
            return;
        for (int i = view->messages.size() - 1; i >= 0 && next < 0; --i)
            next = view->messages.at(i).visible ? i : -1;
    } else {
        next = current + direction;
        while (next >= 0 && next < view->messages.size() && !view->messages.at(next).visible)
            next += direction;
        if (next >= view->messages.size()) {
            cancelReply();
            return;
        }
        if (next < 0)
            return; // the oldest one loaded stays
    }
    if (next >= 0)
        startReply(browser, view->messages.at(next).block, true);
}

void ChatReplies::replyToLatest()
{
    QWidget*      input   = inputFor(nullptr);
    QTextBrowser* browser = input ? m_chat->chatBrowserFor(input) : m_chat->visibleChatBrowser();
    if (!browser)
        return;
    ensureIndex(browser);
    View* view = viewOf(browser);
    if (!view)
        return;
    for (int i = view->messages.size() - 1; i >= 0; --i) {
        if (view->messages.at(i).visible) {
            startReply(browser, view->messages.at(i).block, true);
            return;
        }
    }
}

// ============================================================================================
// The send window and file drops
// ============================================================================================

QWidget* ChatReplies::composeLine(QWidget* parent, const ChatTarget& target)
{
    if (!m_hasPending || !sameChat(m_pending.target, target))
        return nullptr;
    auto* line = new ComposeReplyLine(m_pending.original.nick, m_pending.snippet, m_pending.media, parent);
    connect(line, &ComposeReplyLine::dropped, this, [this] { cancelReply(); });
    for (int i = m_composeLines.size() - 1; i >= 0; --i) {
        if (!m_composeLines.at(i).line)
            m_composeLines.remove(i); // closed windows' lines
    }
    m_composeLines.append({line, target});
    return line;
}

// The send window says what its files go out as: when the reply changes (the hotkey while it is open) or
// ends (sent, canceled, disconnected), its line follows, so it never shows a reply that isn't sent.
void ChatReplies::updateComposeLines()
{
    for (int i = m_composeLines.size() - 1; i >= 0; --i) {
        const ComposeLine& compose = m_composeLines.at(i);
        if (compose.line && m_hasPending && sameChat(m_pending.target, compose.target)) {
            compose.line->setReply(m_pending.original.nick, m_pending.snippet, m_pending.media);
            continue;
        }
        if (compose.line)
            compose.line->deleteLater(); // the window lays itself out again
        m_composeLines.remove(i);
    }
}

QString ChatReplies::leadFor(const ChatTarget& target) const
{
    if (!m_hasPending || !sameChat(m_pending.target, target))
        return {};
    return replies::quoteLine(m_pending.original);
}

void ChatReplies::sentWithFiles(const ChatTarget& target)
{
    if (m_hasPending && sameChat(m_pending.target, target))
        cancelReply();
}

// ============================================================================================
// Jumping to a message
// ============================================================================================

void ChatReplies::activateLine(QTextBrowser* browser, int message)
{
    View*     view     = viewOf(browser);
    const int original = view ? view->originalOf.value(message, -1) : -1;
    if (original < 0) {
        tip(browser->viewport(), i18n::t("The original message isn't in this chat any more."), false);
        return;
    }
    jumpTo(browser, view->messages.at(original).block);
}

void ChatReplies::jumpToPending()
{
    if (!m_hasPending || !m_pending.browser)
        return;
    QTextBrowser* browser = m_pending.browser;
    ensureIndex(browser);
    View* view = viewOf(browser);
    int   idx  = view ? view->byBlock.value(m_pending.block, -1) : -1;
    // TeamSpeak may have dropped old lines meanwhile: find the message again by what it was.
    if (idx < 0 || view->messages.at(idx).nick != m_pending.original.nick || view->messages.at(idx).text != m_pending.original.text) {
        idx = -1;
        for (int i = view ? view->messages.size() - 1 : -1; i >= 0 && idx < 0; --i) {
            const replydoc::Message& m = view->messages.at(i);
            if (m.nick == m_pending.original.nick && m.minutes == m_pending.original.minutes && m.text == m_pending.original.text)
                idx = i;
        }
    }
    if (idx < 0) {
        tip(m_bar ? static_cast<QWidget*>(m_bar.data()) : browser->viewport(), i18n::t("That message isn't in this chat any more."), false);
        return;
    }
    m_pending.block = view->messages.at(idx).block;
    jumpTo(browser, m_pending.block);
}

void ChatReplies::jumpTo(QTextBrowser* browser, int block)
{
    QTextDocument* doc = browser->document();
    QTextBlock     tb  = doc->findBlockByNumber(block);
    while (tb.isValid() && !tb.isVisible()) // a later message of an album: its grid is in the first one
        tb = tb.previous();
    if (!tb.isValid())
        return;
    const QRectF r       = doc->documentLayout()->blockBoundingRect(tb);
    QScrollBar*  bar     = browser->verticalScrollBar();
    const int    height  = browser->viewport()->height();
    const int    current = bar->value();
    int          target  = current;
    if (r.top() < current || r.bottom() > current + height) {
        target = r.height() >= height - 16 ? qRound(r.top()) - 8 : qRound(r.top() - (height - r.height()) / 2.0);
        target = qBound(bar->minimum(), target, bar->maximum());
    }
    const int                    number = tb.blockNumber();
    const QPointer<QTextBrowser> guard(browser);
    if (m_scroll) {
        m_scroll->stop();
        m_scroll->deleteLater();
        m_scroll = nullptr;
    }
    if (target == current || !ui::animationsEnabled()) {
        bar->setValue(target);
        flash(browser, number);
        return;
    }
    // A short glide (decelerating), so it's clear where in the chat the message is.
    m_scroll = new QVariantAnimation(this);
    m_scroll->setStartValue(current);
    m_scroll->setEndValue(target);
    m_scroll->setDuration(kScrollMs);
    m_scroll->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_scroll, &QVariantAnimation::valueChanged, this, [guard](const QVariant& value) {
        if (guard)
            guard->verticalScrollBar()->setValue(value.toInt());
    });
    QVariantAnimation* animation = m_scroll;
    connect(m_scroll, &QVariantAnimation::finished, this, [this, guard, number, animation] {
        if (m_scroll == animation) {
            m_scroll->deleteLater();
            m_scroll = nullptr;
        }
        if (guard)
            flash(guard.data(), number);
    });
    m_scroll->start();
}

void ChatReplies::flash(QTextBrowser* browser, int block)
{
    if (m_flash)
        delete m_flash.data();
    const QPointer<QTextBrowser> guard(browser);
    auto area = [guard, block]() -> QRect {
        if (!guard)
            return {};
        QTextDocument*   doc = guard->document();
        const QTextBlock tb  = doc->findBlockByNumber(block);
        if (!tb.isValid() || !tb.isVisible())
            return {};
        const QRectF r = doc->documentLayout()->blockBoundingRect(tb);
        return QRect(0, qRound(r.top()) - guard->verticalScrollBar()->value(), guard->viewport()->width(), qRound(r.height()));
    };
    const Style style = styleOf(browser);
    m_flash           = new Flash(browser->viewport(), area, replyart::paletteFor(style.dark, style.base).accent, ui::animationsEnabled());
}

// Feedback at the pointer (a click) or at the widget (a key).
void ChatReplies::tip(QWidget* widget, const QString& text, bool error, bool atPointer) const
{
    if (!widget || text.isEmpty())
        return;
    if (atPointer) {
        m_chat->showFeedback(widget, text, error);
        return;
    }
    QToolTip::showText(widget->mapToGlobal(QPoint(12, -6)), text, widget, QRect(), error ? qMax(4000, ui::notificationDurationMs()) : 2500);
}
