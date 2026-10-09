#include "presenceline.h"

#include <QAccessible>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

#include "i18n.h"
#include "peerhub.h"
#include "peers.h"
#include "reactionart.h"
#include "uiutil.h"

PresenceLine::PresenceLine(const ChatTarget& target, QWidget* parent)
    : QWidget(parent)
    , m_target(target)
{
    setObjectName(QString::fromLatin1("tsmediaPresenceLine"));

    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    m_icon->setAccessibleName(QString()); // decorative

    m_text = new QLabel(this);
    m_text->setTextFormat(Qt::PlainText); // nicknames are shown literally
    m_text->setWordWrap(true);
    m_text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    reserveTwoLines();
    QSizePolicy policy = m_text->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_text->setSizePolicy(policy);

    m_who = new QToolButton(this);
    m_who->setText(i18n::t("Who?"));
    m_who->setAccessibleName(i18n::t("Show who has TS Media"));
    m_who->setCheckable(true);
    m_who->setAutoRaise(true);
    m_who->setCursor(Qt::PointingHandCursor);
    m_who->setFocusPolicy(Qt::StrongFocus);
    // A link-styled button; style sheets keep their text past the DLL, hence fromLatin1.
    m_who->setStyleSheet(QString::fromLatin1("QToolButton { border: none; padding: 0 2px; color: palette(link); text-decoration: underline; }"
                                             "QToolButton:focus { border: 1px solid palette(highlight); border-radius: 2px; }"));

    m_details = new QLabel(this);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setWordWrap(true);
    m_details->setVisible(false);
    connect(m_who, &QToolButton::toggled, this, [this](bool on) {
        m_details->setVisible(on && !m_details->text().isEmpty());
        m_who->setAccessibleName(on ? i18n::t("Hide who has TS Media") : i18n::t("Show who has TS Media"));
    });

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addWidget(m_icon, 0, Qt::AlignTop);
    row->addWidget(m_text, 1);
    row->addWidget(m_who, 0, Qt::AlignTop);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addLayout(row);
    auto* detailsRow = new QHBoxLayout;
    detailsRow->setContentsMargins(16 + 6, 0, 0, 0); // under the text, not the icon
    detailsRow->addWidget(m_details);
    layout->addLayout(detailsRow);

    if (PeerHub* hub = PeerHub::instance()) {
        connect(hub, &PeerHub::presenceChanged, this, [this](quint64 sch) {
            if (sch == m_target.sch)
                refresh();
        });
    }
    updateIcon();
    refresh();
}

void PresenceLine::setTarget(const ChatTarget& target)
{
    m_target = target;
    refresh();
}

QString PresenceLine::text() const
{
    return m_shown ? m_text->text() : QString();
}

void PresenceLine::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateIcon();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        reserveTwoLines();
}

void PresenceLine::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    reserveTwoLines(); // style sheets set the label's font when it is polished, before it shows
}

// Two lines from the start: the text changes while the window is open, the buttons below don't move.
void PresenceLine::reserveTwoLines()
{
    m_text->ensurePolished();
    const QFontMetrics fm(m_text->font());
    const int          twoLines = fm.boundingRect(QRect(0, 0, 4096, 4096), Qt::TextWordWrap, QStringLiteral("Xg\nXg")).height();
    const QMargins     margins  = m_text->contentsMargins();
    m_text->setMinimumHeight(twoLines + margins.top() + margins.bottom() + 2 * m_text->margin());
}

void PresenceLine::updateIcon()
{
    const qreal dpr = devicePixelRatioF();
    QPixmap     pixmap(QSize(16, 16) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    // Secondary text colour: the text colour, softened, still 3:1 on the window.
    const QColor text   = palette().color(QPalette::WindowText);
    const QColor window = palette().color(QPalette::Window);
    QColor       icon   = ui::flatten(QColor(text.red(), text.green(), text.blue(), 190), window);
    if (ui::contrastRatio(icon, window) < 3.0)
        icon = text;
    rx::drawPeopleGlyph(p, QRectF(0, 0, 16, 16), icon);
    p.end();
    m_icon->setPixmap(pixmap);
}

void PresenceLine::refresh()
{
    PeerHub*               hub = PeerHub::instance();
    peers::PresenceSummary summary;
    if (hub && hub->directory())
        summary = hub->directory()->summary(m_target.sch, m_target.mode, m_target.clientId);
    showSummary(summary);
}

void PresenceLine::showSummary(const peers::PresenceSummary& summary)
{
    const QString text = peers::presenceText(summary);
    // Presence off, or it can't work on this server: no line at all. The parts are hidden rather than
    // the line itself (a parentless widget made visible would become a window of its own).
    const bool show = !text.isEmpty();
    m_shown         = show;
    m_icon->setVisible(show);
    m_text->setVisible(show);
    setMaximumHeight(show ? QWIDGETSIZE_MAX : 0);
    if (!show) {
        m_who->setVisible(false);
        m_details->setVisible(false);
        return;
    }
    const bool changed = text != m_text->text();
    m_text->setText(text);
    const QStringList details = peers::presenceDetails(summary);
    m_details->setText(details.join(QLatin1Char('\n')));
    // "Who?" only where there is a list to show (people in a channel).
    const bool list = summary.kind == peers::PresenceSummary::Kind::Channel && summary.others() > 0;
    m_who->setVisible(list);
    if (!list && m_who->isChecked())
        m_who->setChecked(false);
    m_details->setVisible(list && m_who->isChecked() && !details.isEmpty());
    if (changed) {
        // One complete sentence for screen readers, not just the number that changed.
        m_text->setAccessibleName(text);
        QAccessibleEvent event(m_text, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}
