#include "replybar.h"

#include <QFontInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolTip>

#include <cmath>

#include "chatscroll.h"
#include "emojirender.h"
#include "i18n.h"
#include "replyart.h"

namespace {

constexpr qreal kGlyph     = 14.0;
constexpr qreal kTextX     = 32.0;
constexpr qreal kClose     = 22.0;
constexpr qreal kClosePad  = 6.0;

QColor mixed(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}

// "Replying to " + name, then the snippet (a file: the clip and its name), elided into width. 2.7 F4: s scales the
// glyph and the gaps with the text.
void drawReplyText(QPainter& p, const QRectF& box, const QFont& font, const QString& nick, const QColor& nameColor, bool underline,
                   const QString& snippet, bool media, const QColor& muted, const QString& separator, qreal s = 1.0)
{
    QFont bold(font);
    bold.setWeight(QFont::DemiBold);
    bold.setUnderline(underline);
    const QFontMetricsF fm(font), bm(bold);
    const qreal         baseline = std::round(box.center().y() + (fm.ascent() - fm.descent()) / 2.0);
    qreal               x        = box.left();
    const QString       lead     = i18n::t("Replying to ");
    p.setFont(font);
    p.setPen(muted);
    p.drawText(QPointF(x, baseline), fm.elidedText(lead, Qt::ElideRight, box.right() - x));
    x += fm.horizontalAdvance(lead);
    if (x >= box.right())
        return;
    // The name keeps at least half of what is left when there is a snippet.
    const qreal   nameRoom = snippet.isEmpty() ? box.right() - x : qMax((box.right() - x) * 0.5, qMin(bm.horizontalAdvance(nick), box.right() - x - 60.0 * s));
    const QString name     = bm.elidedText(nick, Qt::ElideRight, qMax(0.0, nameRoom));
    p.setFont(bold);
    p.setPen(nameColor);
    replyart::drawRun(p, x, baseline, bm, name);
    x += bm.horizontalAdvance(name);
    if (snippet.isEmpty() || box.right() - x < 30.0 * s)
        return;
    p.setFont(font);
    p.setPen(muted);
    p.drawText(QPointF(x, baseline), separator);
    x += fm.horizontalAdvance(separator);
    if (media) {
        const qreal g = 12.0 * s;
        replyart::drawGlyph(p, replyart::Glyph::Clip, QRectF(x, box.center().y() - g / 2.0, g, g), muted);
        x += g + 3.0 * s;
    }
    replyart::drawRun(p, x, baseline, fm, fm.elidedText(snippet, Qt::ElideRight, qMax(0.0, box.right() - x)));
}

// 2.2 emoji: emoji the worker draws later (emojitext.h) show once they are there.
void repaintOnNewPictures(QWidget* widget)
{
    if (emoji::ImageNotifier* notifier = emoji::notifier())
        QObject::connect(notifier, &emoji::ImageNotifier::imagesReady, widget, QOverload<>::of(&QWidget::update));
}

} // namespace

namespace replybar {

QFont widgetFont(const QFont& base, qreal factor, qreal scale)
{
    QFont font(base);
    font.setPixelSize(qMax(10, qRound(11.0 * factor * qMax(0.1, scale))));
    return font;
}

qreal inputScale(const QFont& inputFont)
{
    // TeamSpeak's input font is in points: its pixels at the screen's logical DPI (dpr 1 in TeamSpeak: 11 px at 100%,
    // 22 px at 200%), as QFontInfo resolves it.
    int px = inputFont.pixelSize();
    if (px <= 0 && inputFont.pointSizeF() > 0) {
        const QScreen* screen = QGuiApplication::primaryScreen();
        px                    = qRound(inputFont.pointSizeF() * (screen ? screen->logicalDotsPerInchY() : 96.0) / 72.0);
    }
    if (px <= 0)
        px = QFontInfo(inputFont).pixelSize();
    return qMax(1.0, px / 11.0);
}

bool restyleInLayout(QWidget* bar, bool floating, QTextBrowser* chat, bool keepEnd, const std::function<void()>& change)
{
    if (!bar) {
        if (change)
            change();
        return false;
    }
    const bool bottom = keepEnd && chat && chatscroll::atBottom(chat);
    const int  before = bar->sizeHint().height();
    if (change)
        change();
    if (floating || bar->isHidden() || bar->sizeHint().height() == before)
        return false;
    // The chat above gets its new height now, not after Qt's posted LayoutRequest (a place restored before that would be
    // short of the end by the bar's growth).
    if (QWidget* holder = bar->parentWidget(); holder && holder->layout())
        holder->layout()->activate();
    if (bottom) {
        // Like showBar and the music bar's keepAtEnd: at the end now, and once more after anything Qt posted.
        chat->verticalScrollBar()->setValue(chat->verticalScrollBar()->maximum());
        const QPointer<QTextBrowser> guard(chat);
        QTimer::singleShot(0, bar, [guard] { // a posted event, freed with the bar (ownedtimer.h)
            if (guard)
                guard->verticalScrollBar()->setValue(guard->verticalScrollBar()->maximum());
        });
    }
    return true;
}

} // namespace replybar

// ============================================================================================
// ReplyBar
// ============================================================================================

ReplyBar::ReplyBar(QWidget* parent)
    : QWidget(parent)
{
    // fromLatin1: it lives in TeamSpeak's chat widget; plugin shutdown finds it by this name.
    setObjectName(QString::fromLatin1("tsmediaReplyBar"));
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setLayoutDirection(Qt::LeftToRight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleDescription(i18n::t("Press Enter to send your reply, or Esc to cancel it."));
    repaintOnNewPictures(this);
}

void ReplyBar::setReply(const QString& nick, const QColor& nickColor, const QString& snippet, bool media)
{
    m_nick      = nick;
    m_nickColor = nickColor;
    m_snippet   = snippet;
    m_media     = media;
    setAccessibleName(i18n::t("Replying to %1").arg(nick));
    update();
}

void ReplyBar::setTheme(bool dark, const QColor& base, const QFont& font, qreal scale)
{
    const qreal s      = qBound(1.0, scale, 4.0);
    const QFont scaled = replybar::widgetFont(font, 0.92, s); // 2.7 F4: 10 px at 100%, 20 px at 200%
    if (dark == m_dark && base == m_base && scaled == this->font() && qFuzzyCompare(s, m_scale))
        return; // checked while the bar shows: nothing to lay out again
    m_dark  = dark;
    m_base  = base;
    m_scale = s;
    setFont(scaled);
    updateGeometry();
    update();
}

QSize ReplyBar::sizeHint() const
{
    const int height = qMax(qRound(26 * m_scale), static_cast<int>(std::ceil(QFontMetricsF(font()).height() + 12 * m_scale)));
    return QSize(qRound(240 * m_scale), height);
}

QSize ReplyBar::minimumSizeHint() const
{
    return QSize(qRound(120 * m_scale), sizeHint().height());
}

QRectF ReplyBar::closeRect() const
{
    const qreal close = kClose * m_scale, pad = kClosePad * m_scale;
    return QRectF(width() - pad - close, (height() - close) / 2.0, close, close);
}

QRectF ReplyBar::textRect() const
{
    const qreal left = (kTextX - 22.0) * m_scale;
    return QRectF(left, 0, qMax(0.0, closeRect().left() - 8.0 * m_scale - left), height());
}

ReplyBar::Part ReplyBar::partAt(const QPoint& pos) const
{
    const qreal grace = 3.0 * m_scale;
    if (closeRect().adjusted(-grace, -grace, grace, grace).contains(pos))
        return Part::Close;
    if (textRect().contains(pos))
        return Part::Text;
    return Part::None;
}

void ReplyBar::setHover(Part part)
{
    if (part == m_hover)
        return;
    m_hover = part;
    setCursor(part == Part::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
    update();
}

void ReplyBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const replyart::Palette pal = replyart::paletteFor(m_dark, m_base);
    const QRectF            r   = rect();
    const qreal             s   = m_scale; // 2.7 F4
    const qreal             rad = 6.0 * s;
    QPainterPath            bg;
    bg.moveTo(r.left(), r.bottom() + 1);
    bg.lineTo(r.left(), r.top() + rad);
    bg.quadTo(r.left(), r.top(), r.left() + rad, r.top());
    bg.lineTo(r.right() + 1 - rad, r.top());
    bg.quadTo(r.right() + 1, r.top(), r.right() + 1, r.top() + rad);
    bg.lineTo(r.right() + 1, r.bottom() + 1);
    bg.closeSubpath();
    p.fillPath(bg, pal.surface);

    const QColor muted = replyart::readable(pal.muted, pal.surface, pal.muted);
    replyart::drawGlyph(p, replyart::Glyph::Reply, QRectF(10.0 * s, (height() - kGlyph * s) / 2.0, kGlyph * s, kGlyph * s), muted);
    const QColor name = replyart::readable(m_nickColor, pal.surface, pal.name);
    drawReplyText(p, QRectF(kTextX * s, 0, qMax(0.0, closeRect().left() - 10.0 * s - kTextX * s), height()), font(), m_nick, name, m_hover == Part::Text,
                  m_snippet, m_media, muted, QStringLiteral("  "), s);

    const QRectF close = closeRect();
    if (m_hover == Part::Close || m_pressed == Part::Close) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_pressed == Part::Close ? mixed(pal.hover, pal.text, 0.12) : pal.hover);
        p.drawEllipse(close);
    }
    replyart::drawGlyph(p, replyart::Glyph::Close, close.adjusted(5 * s, 5 * s, -5 * s, -5 * s), m_hover == Part::Close ? pal.text : muted);
}

void ReplyBar::mouseMoveEvent(QMouseEvent* event)
{
    setHover(partAt(event->pos()));
    QWidget::mouseMoveEvent(event);
}

void ReplyBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = partAt(event->pos());
        update();
    }
    event->accept();
}

void ReplyBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Part pressed = m_pressed;
    m_pressed          = Part::None;
    update();
    if (pressed == Part::None || partAt(event->pos()) != pressed)
        return;
    if (pressed == Part::Close)
        emit canceled();
    else
        emit jumpRequested();
}

void ReplyBar::leaveEvent(QEvent* event)
{
    setHover(Part::None);
    QWidget::leaveEvent(event);
}

bool ReplyBar::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto*      he   = static_cast<QHelpEvent*>(event);
        const Part part = partAt(he->pos());
        if (part == Part::Close)
            QToolTip::showText(he->globalPos(), i18n::t("Cancel reply (Esc)"), this, closeRect().toAlignedRect());
        else if (part == Part::Text)
            QToolTip::showText(he->globalPos(), i18n::t("Show the message you're replying to"), this, textRect().toAlignedRect());
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

// ============================================================================================
// ComposeReplyLine
// ============================================================================================

ComposeReplyLine::ComposeReplyLine(const QString& nick, const QString& snippet, bool media, QWidget* parent)
    : QWidget(parent)
    , m_nick(nick)
    , m_snippet(snippet)
    , m_media(media)
{
    setObjectName(QString::fromLatin1("tsmediaComposeReply"));
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setLayoutDirection(Qt::LeftToRight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(i18n::t("Replying to %1").arg(nick));
    repaintOnNewPictures(this);
}

void ComposeReplyLine::setReply(const QString& nick, const QString& snippet, bool media)
{
    m_nick    = nick;
    m_snippet = snippet;
    m_media   = media;
    setAccessibleName(i18n::t("Replying to %1").arg(nick));
    update();
}

QSize ComposeReplyLine::sizeHint() const
{
    return QSize(240, qMax(22, static_cast<int>(std::ceil(QFontMetricsF(font()).height())) + 6));
}

QRectF ComposeReplyLine::closeRect() const
{
    const qreal s = 20.0;
    return QRectF(width() - s, (height() - s) / 2.0, s, s);
}

void ComposeReplyLine::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QColor window = palette().color(QPalette::Window);
    const QColor text   = palette().color(QPalette::WindowText);
    const QColor muted  = replyart::readable(mixed(text, window, 0.32), window, text);
    replyart::drawGlyph(p, replyart::Glyph::Reply, QRectF(0, (height() - 14.0) / 2.0, 14.0, 14.0), muted);
    drawReplyText(p, QRectF(20.0, 0, qMax(0.0, closeRect().left() - 26.0), height()), font(), m_nick, text, false, m_snippet, m_media, muted,
                  QStringLiteral(": "));
    const QRectF close = closeRect();
    if (m_hover || m_pressed) {
        p.setPen(Qt::NoPen);
        p.setBrush(mixed(window, text, m_pressed ? 0.20 : 0.12));
        p.drawEllipse(close);
    }
    replyart::drawGlyph(p, replyart::Glyph::Close, close.adjusted(5, 5, -5, -5), m_hover ? text : muted);
}

void ComposeReplyLine::mouseMoveEvent(QMouseEvent* event)
{
    const bool over = closeRect().adjusted(-3, -3, 3, 3).contains(event->pos());
    if (over != m_hover) {
        m_hover = over;
        setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ComposeReplyLine::mousePressEvent(QMouseEvent* event)
{
    m_pressed = event->button() == Qt::LeftButton && closeRect().adjusted(-3, -3, 3, 3).contains(event->pos());
    update();
}

void ComposeReplyLine::mouseReleaseEvent(QMouseEvent* event)
{
    const bool was = m_pressed;
    m_pressed      = false;
    update();
    if (was && event->button() == Qt::LeftButton && closeRect().adjusted(-3, -3, 3, 3).contains(event->pos())) {
        emit dropped();
        deleteLater(); // the send window lays itself out again
    }
}

void ComposeReplyLine::leaveEvent(QEvent* event)
{
    if (m_hover) {
        m_hover = false;
        setCursor(Qt::ArrowCursor);
        update();
    }
    QWidget::leaveEvent(event);
}

bool ComposeReplyLine::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(event);
        if (closeRect().adjusted(-3, -3, 3, 3).contains(he->pos()))
            QToolTip::showText(he->globalPos(), i18n::t("Send without replying"), this, closeRect().toAlignedRect());
        else
            QToolTip::showText(he->globalPos(), i18n::t("Your files are sent as a reply to %1.").arg(m_nick), this, rect());
        return true;
    }
    return QWidget::event(event);
}
