#include "chatemoji.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>

#include <algorithm>

#include "emojidata.h"
#include "emojiinput.h"
#include "emojirender.h"
#include "emojisegment.h"
#include "i18n.h"
#include "settings.h"

namespace {

// What a replaced character was (kept in the picture's format, so restore() can put it back).
constexpr int kOriginalText   = QTextFormat::UserProperty + 0x7460; // QString: the emoji's text
constexpr int kOriginalFormat = QTextFormat::UserProperty + 0x7461; // QTextFormat: the character's format before
constexpr int kEmoticonName   = QTextFormat::UserProperty + 0x7462; // QString: the TeamSpeak emoticon image it replaced
constexpr int kJumboProperty  = QTextFormat::UserProperty + 0x7463; // bool

constexpr int    kJumboPixels   = 48;
constexpr int    kMaxJumbo      = 27;  // like Discord
constexpr int    kMaxPerBlock   = 400; // emoji replaced in one message
constexpr int    kMaxRanges     = 48;  // changed ranges remembered per document (more: the whole document again)
constexpr int    kStepMs        = 6;   // work per step
constexpr qint64 kCopyWindowMs  = 3000;
const char       kScheme[]      = "tsmemoji:";

bool g_mutating = false;

class MutatingScope
{
  public:
    MutatingScope() { g_mutating = true; }
    ~MutatingScope() { g_mutating = false; }
    MutatingScope(const MutatingScope&)            = delete;
    MutatingScope& operator=(const MutatingScope&) = delete;
};

bool isOurs(const QTextCharFormat& format)
{
    return format.isImageFormat() && (format.hasProperty(kOriginalText) || format.hasProperty(kEmoticonName));
}

// "tsmemoji:1f602/22" (not QStringLiteral: the name lives in TeamSpeak's document).
QString resourceName(int id, int px)
{
    return QString::fromLatin1(kScheme) + QString::fromLatin1(emoji::wireCode(id)) + QLatin1Char('/') + QString::number(px);
}

bool parseResource(const QString& name, int* id, int* px)
{
    if (!name.startsWith(QLatin1String(kScheme)))
        return false;
    const QString rest  = name.mid(static_cast<int>(sizeof(kScheme)) - 1);
    const int     slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return false;
    bool ok = false;
    *id     = emoji::fromWireCode(rest.left(slash).toLatin1());
    *px     = rest.mid(slash + 1).toInt(&ok);
    return ok && *id >= 0 && *px > 0 && *px <= 128;
}

int idOf(const QTextCharFormat& format)
{
    int id = -1;
    int px = 0;
    return parseResource(format.toImageFormat().name(), &id, &px) ? id : -1;
}

// What the character stood for in text: the emoji, or the emoticon's code (":)").
QString originalOf(const QTextCharFormat& format)
{
    if (format.hasProperty(kOriginalText))
        return format.stringProperty(kOriginalText);
    if (format.hasProperty(kEmoticonName))
        return emoji::teamSpeakEmoticonCode(format.stringProperty(kEmoticonName));
    return {};
}

// Inline emoji: 1.375 x the text (Discord's 22 px at 16 px), but never taller than the line, so a line
// with emoji is exactly as high as without.
int inlinePixels(const QFont& font)
{
    const int px   = QFontInfo(font).pixelSize();
    const int line = QFontMetrics(font).height();
    return qBound(12, qMin(qRound((px > 0 ? px : 12) * 1.375), line > 0 ? line : 40), 40);
}

bool atBottom(QTextBrowser* browser)
{
    const QScrollBar* bar = browser->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4;
}

QString normalizedText(QString text)
{
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    text.replace(QChar::Nbsp, QLatin1Char(' '));
    return text;
}

bool selectionHasEmoji(const QTextCursor& selection)
{
    if (!selection.hasSelection())
        return false;
    QTextDocument* doc  = selection.document();
    const int      from = selection.selectionStart();
    const int      to   = selection.selectionEnd();
    for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() < to; b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.position() < to && f.position() + f.length() > from && isOurs(f.charFormat()))
                return true;
        }
    }
    return false;
}

} // namespace

ChatEmoji::ChatEmoji(QObject* parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ChatEmoji::step);
    m_input = new EmojiInput(this);
    if (emoji::ImageNotifier* n = emoji::notifier())
        connect(n, &emoji::ImageNotifier::imagesReady, this, [this] { schedule(0); });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ChatEmoji::fixClipboard);
    m_lastEnabled = enabled();
}

ChatEmoji::~ChatEmoji()
{
    m_timer->stop();
    restoreAll();
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser) {
            it->browser->removeEventFilter(this);
            it->browser->viewport()->removeEventFilter(this);
        }
    }
    delete m_input; // gives the chat inputs their space back
    m_input = nullptr;
}

bool ChatEmoji::isMutating()
{
    return g_mutating;
}

bool ChatEmoji::enabled() const
{
    return Settings::instance().hdEmoji && emoji::hasColor();
}

ChatEmoji::View* ChatEmoji::viewFor(QTextBrowser* browser)
{
    auto it = m_views.find(browser);
    return it == m_views.end() ? nullptr : &it.value();
}

void ChatEmoji::attach(QTextBrowser* browser)
{
    if (!browser || m_views.contains(browser))
        return;
    View view;
    view.browser  = browser;
    view.document = browser->document();
    m_views.insert(browser, view);
    connect(browser, &QObject::destroyed, this, [this, browser] { m_views.remove(browser); });
    browser->installEventFilter(this);
    browser->viewport()->installEventFilter(this);
    View& v = m_views[browser];
    watch(v);
    restart(v);
}

void ChatEmoji::attachInput(QTextEdit* input)
{
    if (m_input)
        m_input->attach(input);
}

void ChatEmoji::documentSwapped(QTextBrowser* browser)
{
    View* view = viewFor(browser);
    if (!view || view->document == browser->document())
        return;
    view->document = browser->document();
    view->resources.clear();
    view->redraw.clear();
    watch(*view);
    restart(*view);
}

void ChatEmoji::watch(View& view)
{
    QTextDocument* doc = view.document;
    if (!doc)
        return;
    QPointer<QTextBrowser> browser = view.browser;
    connect(doc, &QTextDocument::contentsChange, this, [this, doc, browser](int position, int, int added) {
        if (g_mutating || !browser)
            return;
        View* v = viewFor(browser.data());
        if (!v || v->document != doc || !enabled())
            return;
        if (v->dirty.size() >= kMaxRanges) {
            restart(*v); // too much at once: the whole document again (what is done is quick to skip)
            return;
        }
        Range range;
        range.from = QTextCursor(doc);
        range.from.setPosition(qBound(0, position, doc->characterCount() - 1));
        range.from.setKeepPositionOnInsert(true);
        range.to = QTextCursor(doc);
        range.to.setPosition(qBound(0, position + added, doc->characterCount() - 1));
        v->dirty.append(range);
        schedule();
    });
}

void ChatEmoji::restart(View& view)
{
    view.dirty.clear();
    view.history = QTextCursor();
    if (!view.document || !enabled())
        return;
    view.history = QTextCursor(view.document);
    view.history.movePosition(QTextCursor::End);
    schedule(0);
}

void ChatEmoji::schedule(int delayMs)
{
    if (!m_timer->isActive() || delayMs < m_timer->remainingTime())
        m_timer->start(delayMs);
}

void ChatEmoji::settingsChanged()
{
    const bool on = enabled();
    if (m_input)
        m_input->settingsChanged();
    // Turned off, or the jumbo rule changed: everything back, then (when on) done again.
    restoreAll();
    m_lastEnabled = on;
    if (!on)
        return;
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        restart(it.value());
}

void ChatEmoji::restoreAll()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        View& view = it.value();
        view.dirty.clear();
        view.history = QTextCursor();
        view.waiting = false;
        if (!view.document || !view.browser)
            continue;
        const bool bottom = atBottom(view.browser);
        restore(view.document);
        // The pictures' memory: the document keeps the names, with nothing behind them.
        for (const QString& name : qAsConst(view.resources))
            view.document->addResource(QTextDocument::ImageResource, QUrl(name), QVariant());
        view.resources.clear();
        view.redraw.clear();
        if (bottom)
            view.browser->verticalScrollBar()->setValue(view.browser->verticalScrollBar()->maximum());
    }
}

// ---- the steps ---------------------------------------------------------------------------------------

void ChatEmoji::step()
{
    if (!enabled())
        return;
    QElapsedTimer clock;
    clock.start();
    bool more = false;
    // Chats on screen first.
    QVector<View*> order;
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (it->browser && it->document == it->browser->document())
            order.append(&it.value());
    }
    std::stable_sort(order.begin(), order.end(), [](const View* a, const View* b) { return a->browser->isVisible() && !b->browser->isVisible(); });
    for (View* view : qAsConst(order)) {
        if (clock.elapsed() >= kStepMs) {
            more = true;
            break;
        }
        more = stepView(*view, qMax<qint64>(1, kStepMs - clock.elapsed()), false) || more;
    }
    if (more)
        schedule(15);
}

void ChatEmoji::processNow(QTextBrowser* browser)
{
    View* view = viewFor(browser);
    if (!view || !enabled())
        return;
    while (stepView(*view, -1, true)) {
    }
}

bool ChatEmoji::stepView(View& view, qint64 budgetMs, bool synchronous)
{
    QTextBrowser*  browser = view.browser;
    QTextDocument* doc     = view.document;
    if (!browser || !doc)
        return false;
    if (view.dirty.isEmpty() && view.history.isNull() && view.redraw.isEmpty() && view.dpr == browser->devicePixelRatioF())
        return false;
    QElapsedTimer clock;
    clock.start();
    const auto overBudget = [&] { return budgetMs >= 0 && clock.elapsed() >= budgetMs; };

    // Another screen (device pixel ratio): the pictures are drawn again at the new one.
    const qreal dpr = browser->devicePixelRatioF();
    if (view.dpr != dpr) {
        view.dpr    = dpr;
        view.redraw = view.resources;
    }
    if (!view.redraw.isEmpty()) {
        const QSet<QString> names = view.redraw;
        for (const QString& name : names) {
            int id = -1;
            int px = 0;
            if (!parseResource(name, &id, &px)) {
                view.redraw.remove(name);
                continue;
            }
            const QImage image = synchronous ? emoji::render(id, px, dpr) : emoji::requestImage(id, px, dpr, false);
            if (image.isNull())
                continue; // on its way
            doc->addResource(QTextDocument::ImageResource, QUrl(name), image);
            view.redraw.remove(name);
        }
        browser->viewport()->update();
    }

    // Where the reader is, to keep it there.
    QScrollBar*  bar    = browser->verticalScrollBar();
    const bool   bottom = atBottom(browser);
    QTextCursor  anchor(doc);
    qreal        offset = 0;
    {
        const int top = doc->documentLayout()->hitTest(QPointF(4, bar->value()), Qt::FuzzyHit);
        anchor.setPosition(qBound(0, top, doc->characterCount() - 1));
        anchor.movePosition(QTextCursor::StartOfBlock);
        offset = doc->documentLayout()->blockBoundingRect(anchor.block()).top() - bar->value();
    }
    bool changed = false;
    bool waiting = false;

    // Changed lines (newest first).
    while (!view.dirty.isEmpty() && !overBudget()) {
        Range&     range = view.dirty.last();
        QTextBlock first = range.from.block();
        QTextBlock last  = range.to.block();
        if (!first.isValid() || !last.isValid() || last.blockNumber() < first.blockNumber()) {
            view.dirty.removeLast();
            continue;
        }
        bool done   = true;
        view.urgent = true; // new messages: their pictures before the history's
        for (QTextBlock b = last; b.isValid() && b.blockNumber() >= first.blockNumber(); b = b.previous()) {
            bool blockChanged = false;
            if (!processBlock(view, b, synchronous, &blockChanged)) {
                done = false;
                // Lines after it are done: the range ends before them now.
                range.to.setPosition(b.position() + b.length() - 1);
                break;
            }
            changed = changed || blockChanged;
            if (overBudget() && b.blockNumber() > first.blockNumber()) {
                range.to.setPosition(qMax(0, b.position() - 1));
                done = false;
                break;
            }
        }
        if (done) {
            view.dirty.removeLast();
        } else if (!overBudget()) {
            waiting = true; // pictures on their way: the next step goes on with it
            break;
        }
    }

    // The history, backwards.
    view.urgent = false;
    while (!view.history.isNull() && !waiting && !overBudget()) {
        const QTextBlock b = view.history.block();
        if (!b.isValid()) {
            view.history = QTextCursor();
            break;
        }
        bool blockChanged = false;
        if (!processBlock(view, b, synchronous, &blockChanged)) {
            waiting = true;
            break;
        }
        changed = changed || blockChanged;
        const QTextBlock previous = b.previous();
        if (!previous.isValid()) {
            view.history = QTextCursor();
            break;
        }
        view.history.setPosition(previous.position());
    }
    view.waiting = waiting;

    if (changed) {
        if (bottom) {
            bar->setValue(bar->maximum());
        } else if (anchor.block().isValid()) {
            const qreal top = doc->documentLayout()->blockBoundingRect(anchor.block()).top();
            bar->setValue(qRound(top - offset));
        }
    }
    const bool workLeft = !view.dirty.isEmpty() || !view.history.isNull();
    // Waiting for pictures: the worker's signal brings the next step (no polling).
    return workLeft && !waiting;
}

bool ChatEmoji::processBlock(View& view, const QTextBlock& block, bool synchronous, bool* changed)
{
    *changed           = false;
    QTextDocument* doc = view.document;
    struct Candidate {
        int             position = 0;
        int             length   = 0;
        int             id       = -1;
        QTextCharFormat format;
        QString         text;
        bool            emoticon = false;
        bool            message  = false; // in the message part (after "Nick": )
    };
    QVector<Candidate> todo;
    int  messageStart   = -1; // after the nickname link and its ": "
    bool header         = false;
    bool otherInMessage = false; // links, other objects or text in the message part
    int  existing       = 0;     // HD emoji already in the message part

    for (auto it = block.begin(); !it.atEnd() && todo.size() < kMaxPerBlock; ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf  = f.charFormat();
        const int             pos = f.position();
        const QString         href = cf.anchorHref();
        if (cf.isAnchor() || !href.isEmpty()) {
            if (href.contains(QLatin1String("ts3file"), Qt::CaseInsensitive))
                break; // the TS Media link and the "plugin required" note after it stay as they are
            if (!header && href.startsWith(QLatin1String("client://"), Qt::CaseInsensitive)) {
                header       = true;
                messageStart = pos + f.length();
            } else if (header) {
                otherInMessage = true;
            }
            continue;
        }
        const bool inMessage = header && pos >= messageStart;
        if (cf.isImageFormat()) {
            if (isOurs(cf)) {
                existing += inMessage ? f.length() : 0;
                continue;
            }
            const int id = emoji::forTeamSpeakEmoticon(cf.toImageFormat().name());
            if (id >= 0 && emoji::supported(id)) {
                for (int i = 0; i < f.length(); ++i)
                    todo.append({pos + i, 1, id, cf, QString(), true, inMessage});
            } else if (inMessage) {
                otherInMessage = true;
            }
            continue;
        }
        const QString               text    = f.text();
        const QVector<emoji::Match> matches = emoji::findEmoji(text, kMaxPerBlock - todo.size());
        int                         covered = 0; // text before this index is accounted for
        for (const emoji::Match& m : matches) {
            if (inMessage && !otherInMessage) {
                QString between = text.mid(covered, m.start - covered);
                if (pos + covered == messageStart && between.startsWith(QLatin1Char(':')))
                    between.remove(0, 1);
                if (!between.trimmed().isEmpty())
                    otherInMessage = true;
            }
            covered = m.start + m.length;
            if (!emoji::showsAsPicture(m) || !emoji::supported(m.id)) {
                if (inMessage)
                    otherInMessage = true;
                continue;
            }
            todo.append({pos + m.start, m.length, m.id, cf, text.mid(m.start, m.length), false, inMessage});
        }
        if (inMessage && !otherInMessage) {
            QString rest = text.mid(covered);
            if (pos + covered == messageStart && rest.startsWith(QLatin1Char(':')))
                rest.remove(0, 1);
            rest.remove(QChar(0x200D)).remove(QChar(0xFE0F));
            if (!rest.trimmed().isEmpty())
                otherInMessage = true;
        }
    }
    if (todo.isEmpty())
        return true;

    int pictures = existing;
    for (const Candidate& c : qAsConst(todo))
        pictures += c.message ? 1 : 0;
    const bool jumbo = Settings::instance().jumboEmoji && header && !otherInMessage && pictures >= 1 && pictures <= kMaxJumbo && existing == 0;

    // The pictures, all of them before anything changes.
    const qreal dpr     = view.dpr > 0 ? view.dpr : (view.browser ? view.browser->devicePixelRatioF() : 1.0);
    bool        missing = false;
    QVector<int> sizes;
    sizes.reserve(todo.size());
    for (const Candidate& c : qAsConst(todo)) {
        const int px = jumbo && c.message ? kJumboPixels : inlinePixels(c.format.font().resolve(doc->defaultFont()));
        sizes.append(px);
        const QString name = resourceName(c.id, px);
        if (view.resources.contains(name))
            continue;
        const QImage image = synchronous ? emoji::render(c.id, px, dpr) : emoji::requestImage(c.id, px, dpr, view.urgent);
        if (image.isNull()) {
            missing = true;
            continue;
        }
        doc->addResource(QTextDocument::ImageResource, QUrl(name), image);
        view.resources.insert(name);
    }
    if (missing)
        return false;

    MutatingScope mutating;
    QTextCursor   cursor(doc);
    cursor.beginEditBlock();
    for (int i = todo.size() - 1; i >= 0; --i) {
        const Candidate& c  = todo.at(i);
        const int        px = sizes.at(i);
        QTextImageFormat image;
        if (c.emoticon) {
            image = c.format.toImageFormat();
            image.setProperty(kEmoticonName, image.name());
        } else {
            image.merge(c.format);
            image.setObjectType(QTextFormat::ImageObject);
            image.setProperty(kOriginalText, c.text);
        }
        image.setProperty(kOriginalFormat, QTextFormat(c.format));
        image.setName(resourceName(c.id, px));
        image.setWidth(px);
        image.setHeight(px);
        // Inline like Discord's (bottom of the line: no taller lines); jumbo centred on the header's text.
        image.setVerticalAlignment(jumbo && c.message ? QTextCharFormat::AlignMiddle : QTextCharFormat::AlignBottom);
        image.setProperty(kJumboProperty, jumbo && c.message);
        cursor.setPosition(c.position);
        cursor.setPosition(c.position + c.length, QTextCursor::KeepAnchor);
        if (c.emoticon)
            cursor.setCharFormat(image);
        else
            cursor.insertText(QString(QChar::ObjectReplacementCharacter), image);
    }
    cursor.endEditBlock();
    *changed = true;
    return true;
}

void ChatEmoji::processDocument(QTextDocument* document, qreal dpr)
{
    if (!document || !emoji::hasColor())
        return;
    View view;
    view.document = document;
    view.dpr      = dpr;
    for (QTextBlock b = document->lastBlock(); b.isValid(); b = b.previous()) {
        bool changed = false;
        processBlock(view, b, true, &changed);
    }
}

// ---- restoring -----------------------------------------------------------------------------------------

void ChatEmoji::restore(QTextDocument* document)
{
    if (!document)
        return;
    QVector<int> positions;
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && isOurs(f.charFormat())) {
                for (int i = 0; i < f.length(); ++i)
                    positions.append(f.position() + i);
            }
        }
    }
    if (positions.isEmpty())
        return;
    MutatingScope mutating;
    QTextCursor   cursor(document);
    cursor.beginEditBlock();
    for (int i = positions.size() - 1; i >= 0; --i) {
        const int p = positions.at(i);
        cursor.setPosition(p);
        cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat ours     = cursor.charFormat();
        const QTextCharFormat original = qvariant_cast<QTextFormat>(ours.property(kOriginalFormat)).toCharFormat();
        if (ours.hasProperty(kEmoticonName)) {
            QTextCharFormat back = original;
            if (!back.isImageFormat()) { // older state without the whole format: the emoticon's name at least
                QTextImageFormat image;
                image.setName(ours.stringProperty(kEmoticonName));
                back = image;
            }
            cursor.setCharFormat(back);
        } else {
            cursor.insertText(ours.stringProperty(kOriginalText), original);
        }
    }
    cursor.endEditBlock();
}

int ChatEmoji::countEmoji(QTextDocument* document)
{
    int count = 0;
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && isOurs(f.charFormat()))
                count += f.length();
        }
    }
    return count;
}

QString ChatEmoji::originalText(QTextDocument* document, int from, int to)
{
    QString out;
    if (!document || to <= from)
        return out;
    for (QTextBlock b = document->findBlock(from); b.isValid() && b.position() < to; b = b.next()) {
        if (b.position() > from)
            out += QLatin1Char('\n');
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            const int start = qMax(from, f.position());
            const int end   = qMin(to, f.position() + f.length());
            if (start >= end)
                continue;
            const QTextCharFormat cf = f.charFormat();
            if (isOurs(cf)) {
                const QString original = originalOf(cf);
                for (int i = start; i < end; ++i)
                    out += original;
                continue;
            }
            if (cf.isImageFormat()) {
                const QString code = emoji::teamSpeakEmoticonCode(cf.toImageFormat().name()); // TeamSpeak's own emoticons
                for (int i = start; i < end && !code.isEmpty(); ++i)
                    out += code;
                continue;
            }
            QString text = f.text().mid(start - f.position(), end - start);
            text.remove(QChar::ObjectReplacementCharacter);
            out += text;
        }
    }
    return normalizedText(out);
}

// ---- copying --------------------------------------------------------------------------------------

void ChatEmoji::copySelection(QTextBrowser* browser)
{
    const QTextCursor selection = browser->textCursor();
    const QString     text      = originalText(browser->document(), selection.selectionStart(), selection.selectionEnd());
    m_settingClipboard          = true;
    QGuiApplication::clipboard()->setText(text);
    m_settingClipboard = false;
}

// After TeamSpeak's own copy (its menu or shortcut): if the clipboard holds the selection with object
// characters (or none at all) where the HD emoji are, it gets their text instead.
bool ChatEmoji::fixClipboard()
{
    if (m_settingClipboard || !m_copyBrowser || QDateTime::currentMSecsSinceEpoch() - m_copyMs > kCopyWindowMs)
        return false;
    QTextBrowser*     browser   = m_copyBrowser.data();
    const QTextCursor selection = browser->textCursor();
    if (!selectionHasEmoji(selection))
        return false;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasText())
        return false;
    const QString copied = normalizedText(mime->text());
    QString       raw    = normalizedText(QTextDocumentFragment(selection).toPlainText());
    QString       bare   = raw;
    bare.remove(QChar::ObjectReplacementCharacter);
    if (copied != raw && copied != bare && copied.trimmed() != bare.trimmed())
        return false;
    m_copyBrowser.clear();
    copySelection(browser);
    return true;
}

// ---- the pointer ------------------------------------------------------------------------------------

int ChatEmoji::emojiAt(QTextBrowser* browser, const QPoint& viewportPos, int* position, QRect* area) const
{
    QTextDocument* doc = browser->document();
    const QPointF  docPos(viewportPos.x() + browser->horizontalScrollBar()->value(), viewportPos.y() + browser->verticalScrollBar()->value());
    const int      hit  = doc->documentLayout()->hitTest(docPos, Qt::FuzzyHit);
    const int      last = doc->characterCount() - 1;
    for (int p : {hit, hit - 1}) {
        if (p < 0 || p + 1 > last)
            continue;
        QTextCursor c(doc);
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat format = c.charFormat();
        if (!isOurs(format))
            continue;
        const QTextBlock   block  = doc->findBlock(p);
        const QTextLayout* layout = block.layout();
        if (!layout)
            continue;
        const int       rel  = p - block.position();
        const QTextLine line = layout->lineForTextPosition(rel);
        if (!line.isValid())
            continue;
        const QRectF blockRect = doc->documentLayout()->blockBoundingRect(block);
        const qreal  x1        = line.cursorToX(rel);
        const qreal  x2        = line.cursorToX(rel + 1);
        const QRectF rect(blockRect.left() + qMin(x1, x2), blockRect.top() + line.y(), qAbs(x2 - x1), line.height());
        if (!rect.contains(docPos))
            continue;
        if (position)
            *position = p;
        if (area)
            *area = rect.translated(-browser->horizontalScrollBar()->value(), -browser->verticalScrollBar()->value()).toAlignedRect();
        return idOf(format);
    }
    return -1;
}

void ChatEmoji::showMenu(QTextBrowser* browser, int position, int id, const QPoint& globalPos)
{
    auto* menu = new QMenu(browser);
    menu->setObjectName(QString::fromLatin1("tsmediaEmojiMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setLayoutDirection(Qt::LeftToRight);
    QPixmap icon = QPixmap::fromImage(emoji::render(id, 16, browser->devicePixelRatioF()));
    QString name = emoji::name(id);
    if (!name.isEmpty())
        name[0] = name.at(0).toUpper();
    menu->addAction(QIcon(icon), name)->setEnabled(false);
    menu->addSeparator();
    const QPointer<QTextBrowser> guard(browser);
    const QString                text = emoji::text(id);
    menu->addAction(i18n::t("Copy &emoji"), this, [this, text] {
        m_settingClipboard = true;
        QGuiApplication::clipboard()->setText(text);
        m_settingClipboard = false;
    });
    menu->addAction(i18n::t("Copy &text"), this, [this, guard, position] {
        if (!guard)
            return;
        // The selection when the emoji is in it; otherwise the whole message.
        const QTextCursor selection = guard->textCursor();
        int               from      = selection.selectionStart();
        int               to        = selection.selectionEnd();
        if (!selection.hasSelection() || position < from || position >= to) {
            const QTextBlock block = guard->document()->findBlock(position);
            from                   = block.position();
            to                     = block.position() + block.length() - 1;
        }
        m_settingClipboard = true;
        QGuiApplication::clipboard()->setText(originalText(guard->document(), from, to));
        m_settingClipboard = false;
    });
    if (m_input && m_input->hasInput())
        menu->addAction(i18n::t("&Use in the chat input"), this, [this, id] { insertIntoInput(id); });
    menu->popup(globalPos);
}

void ChatEmoji::insertIntoInput(int id)
{
    if (m_input)
        m_input->insertEmoji(id);
}

bool ChatEmoji::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::ShortcutOverride && type != QEvent::ToolTip && type != QEvent::ContextMenu)
        return false;
    QTextBrowser* browser = nullptr;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it) {
        if (it->browser && (it->browser.data() == watched || it->browser->viewport() == watched))
            browser = it->browser.data();
    }
    if (!browser)
        return false;

    if (type == QEvent::KeyPress || type == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ke->matches(QKeySequence::Copy))
            return false;
        if (!selectionHasEmoji(browser->textCursor())) {
            // TeamSpeak's own copy: corrected afterwards if it leaves the emoji out after all.
            m_copyBrowser = browser;
            m_copyMs      = QDateTime::currentMSecsSinceEpoch();
            return false;
        }
        if (type == QEvent::ShortcutOverride) {
            ke->accept(); // the key comes here instead of to a window shortcut
            return true;
        }
        copySelection(browser);
        return true;
    }
    if (!enabled() && type != QEvent::ContextMenu)
        return false;

    if (type == QEvent::ToolTip) {
        auto*     he = static_cast<QHelpEvent*>(event);
        QRect     area;
        const int id = emojiAt(browser, he->pos(), nullptr, &area);
        if (id < 0)
            return false;
        QString name = emoji::name(id);
        if (!name.isEmpty())
            name[0] = name.at(0).toUpper();
        QToolTip::showText(he->globalPos(), name, browser->viewport(), area); // goes when the pointer leaves the emoji
        return true;
    }

    // Context menu: ours on an HD emoji; TeamSpeak's anywhere else (its Copy is corrected afterwards).
    auto*     ce       = static_cast<QContextMenuEvent*>(event);
    int       position = -1;
    const int id       = ce->reason() == QContextMenuEvent::Mouse ? emojiAt(browser, ce->pos(), &position) : -1;
    if (id >= 0) {
        showMenu(browser, position, id, ce->globalPos());
        return true;
    }
    m_copyBrowser = browser;
    m_copyMs      = QDateTime::currentMSecsSinceEpoch();
    return false;
}
