#include "uploadtoast.h"

#include <QBasicTimer>
#include <QCursor>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QTimerEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "uiutil.h"

namespace {

// What QPainter draws with; the labels get the same colours from the style sheets below. Text passes
// 4.5:1 on the toast (the dark one also when it is composited over a white chat), the progress fill
// 3:1 on its track.
struct ToastColors {
    QColor text;
    QColor muted;
    QColor error;
    QColor done;
    QColor accent; // progress fill
    QColor track;
    QColor surface; // the toast's own background (the mark inside the alert glyph)
};

ToastColors toastColors(bool dark)
{
    ToastColors c;
    if (dark) {
        c.text    = QColor(0xf2, 0xf3, 0xf5); // 14.2:1
        c.muted   = QColor(0xb5, 0xba, 0xc1); // 8.1:1
        c.error   = QColor(0xfa, 0x77, 0x7c); // 6.0:1 (5.0:1 over a white chat)
        c.done    = QColor(0x2d, 0xc7, 0x70); // 7.1:1, 5.0:1 on the track
        c.accent  = QColor(0x79, 0x83, 0xf5); // 3.4:1 on the track, 4.8:1 on the toast
        c.track   = QColor(0x3a, 0x3c, 0x42);
        c.surface = QColor(0x21, 0x23, 0x26);
    } else {
        c.text    = QColor(0x06, 0x06, 0x07); // 20:1
        c.muted   = QColor(0x5c, 0x5e, 0x66); // 6.5:1
        c.error   = QColor(0xc4, 0x28, 0x2d); // 5.7:1
        c.done    = QColor(0x17, 0x72, 0x45); // 6.0:1, 4.1:1 on the track
        c.accent  = QColor(0x58, 0x65, 0xf2); // 3.2:1 on the track
        c.track   = QColor(0xd4, 0xd7, 0xdc);
        c.surface = QColor(0xff, 0xff, 0xff);
    }
    return c;
}

// Applied with QString::fromLatin1, never QStringLiteral: the toast lives in TeamSpeak's chat and
// shares its QStyleSheetStyle when TeamSpeak uses style sheets. That style's parser keeps the last
// text it parsed until TeamSpeak destroys it at exit, after this DLL is unloaded, and ~QString on
// literal data of an unloaded DLL crashes TeamSpeak.
const char* const kDarkSheet =
    "#tsmediaUploadToast { background: rgba(32,34,37,238); border: 1px solid rgba(255,255,255,30); border-radius: 8px; }"
    "#tsmediaUploadToast QLabel { color: #f2f3f5; background: transparent; }"
    "#tsmediaUploadToast QLabel[role=\"status\"] { color: #b5bac1; }"
    "#tsmediaUploadToast QLabel[role=\"status\"][state=\"error\"] { color: #fa777c; }"
    "#tsmediaUploadToast QLabel[role=\"status\"][state=\"done\"] { color: #2dc770; }"
    "#tsmediaUploadToast QToolButton { color: #949cf7; background: transparent; border: none; border-radius: 4px; padding: 0px 8px; }"
    "#tsmediaUploadToast QToolButton:hover { background: rgba(255,255,255,24); }"
    "#tsmediaUploadToast QToolButton:pressed { background: rgba(255,255,255,40); }"
    "#tsmediaUploadToast QToolButton[role=\"more\"] { color: #b5bac1; padding: 0px; }"
    "#tsmediaUploadToast QToolButton[role=\"more\"]:hover { color: #f2f3f5; background: transparent; text-decoration: underline; }"
    "#tsmediaUploadToast QToolButton[role=\"glyph\"] { padding: 0px; }"
    "#tsmediaUploadToast QFrame[role=\"divider\"] { background: rgba(255,255,255,30); border: none; }"
    "QToolTip { color: #f2f3f5; background: #111214; border: 1px solid #3f4147; padding: 4px 6px; }";

// TeamSpeak's light theme: a white card. Tooltips keep TeamSpeak's own style.
const char* const kLightSheet =
    "#tsmediaUploadToast { background: rgba(255,255,255,250); border: 1px solid #d4d7dc; border-radius: 8px; }"
    "#tsmediaUploadToast QLabel { color: #060607; background: transparent; }"
    "#tsmediaUploadToast QLabel[role=\"status\"] { color: #5c5e66; }"
    "#tsmediaUploadToast QLabel[role=\"status\"][state=\"error\"] { color: #c4282d; }"
    "#tsmediaUploadToast QLabel[role=\"status\"][state=\"done\"] { color: #177245; }"
    "#tsmediaUploadToast QToolButton { color: #4752c4; background: transparent; border: none; border-radius: 4px; padding: 0px 8px; }"
    "#tsmediaUploadToast QToolButton:hover { background: rgba(0,0,0,20); }"
    "#tsmediaUploadToast QToolButton:pressed { background: rgba(0,0,0,36); }"
    "#tsmediaUploadToast QToolButton[role=\"more\"] { color: #5c5e66; padding: 0px; }"
    "#tsmediaUploadToast QToolButton[role=\"more\"]:hover { color: #060607; background: transparent; text-decoration: underline; }"
    "#tsmediaUploadToast QToolButton[role=\"glyph\"] { padding: 0px; }"
    "#tsmediaUploadToast QFrame[role=\"divider\"] { background: #e3e5e8; border: none; }";

constexpr int kButtonSize = 24; // close, Retry and Cancel all: at least 24 x 24 px

bool isRunning(UploadState state)
{
    return state == UploadState::Preparing || state == UploadState::Uploading;
}

bool isActive(UploadState state)
{
    return isRunning(state) || state == UploadState::Posting;
}

QString dot()
{
    return QStringLiteral(" · ");
}

// Qt shows a tooltip as rich text when it looks like HTML; file names and server messages are text.
QString plainToolTip(const QString& text, bool singleLine)
{
    if (text.isEmpty())
        return text;
    return (singleLine ? QStringLiteral("<p style='white-space:pre'>%1</p>") : QStringLiteral("<p>%1</p>")).arg(text.toHtmlEscaped());
}

void setStyleState(QWidget* widget, const QString& state)
{
    widget->setProperty("state", state); // fromLatin1 values: the style engine may outlive the DLL
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QToolButton* textButton(const QString& text, QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    button->setFixedHeight(kButtonSize);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus); // the toast never takes the focus from the chat input
    return button;
}

} // namespace

// Thin rounded progress line; also has an indeterminate mode for the preparing phase (a still track
// when Windows animations are off).
class UploadToast::ProgressLine : public QWidget
{
  public:
    explicit ProgressLine(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(4);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setProgress(double value)
    {
        value = qBound(0.0, value, 1.0);
        if (!m_busy && qFuzzyCompare(value + 1.0, m_value + 1.0))
            return;
        m_busy = false;
        m_timer.stop();
        m_value = value;
        update();
    }

    void setBusy()
    {
        if (m_busy)
            return;
        m_busy  = true;
        m_phase = 0.0;
        if (isVisible() && ui::animationsEnabled())
            m_timer.start(30, this);
        update();
    }

    void setColors(const QColor& track, const QColor& fill)
    {
        if (track == m_track && fill == m_fill)
            return;
        m_track = track;
        m_fill  = fill;
        update();
    }

  protected:
    void showEvent(QShowEvent*) override
    {
        if (m_busy && ui::animationsEnabled())
            m_timer.start(30, this);
    }

    void hideEvent(QHideEvent*) override
    {
        m_timer.stop();
    }

    void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() != m_timer.timerId()) {
            QWidget::timerEvent(event);
            return;
        }
        if (!ui::animationsEnabled())
            m_timer.stop(); // switched off meanwhile: the still track
        m_phase += 0.018;
        if (m_phase > 1.0)
            m_phase -= 1.0;
        update();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QRectF r(rect());
        const qreal  radius = r.height() / 2.0;
        p.setBrush(m_track);
        p.drawRoundedRect(r, radius, radius);

        QRectF fill;
        if (m_busy) {
            if (!m_timer.isActive())
                return;
            const qreal segment = r.width() * 0.3;
            fill                = QRectF(-segment + (r.width() + segment) * m_phase, 0, segment, r.height()).intersected(r);
        } else {
            fill = QRectF(0, 0, r.width() * m_value, r.height());
        }
        if (fill.width() <= 0.0)
            return;
        p.setBrush(m_fill);
        p.drawRoundedRect(fill, radius, radius);
    }

  private:
    double      m_value = 0.0;
    double      m_phase = 0.0;
    bool        m_busy  = false;
    QColor      m_track = QColor(0x3a, 0x3c, 0x42);
    QColor      m_fill  = QColor(0x79, 0x83, 0xf5);
    QBasicTimer m_timer;
};

// The close/cancel button: a 24 x 24 px target with an X painted as a vector (sharp at any scale);
// hover and pressed backgrounds come from the style sheet.
class UploadToast::GlyphButton : public QToolButton
{
  public:
    explicit GlyphButton(QWidget* parent)
        : QToolButton(parent)
    {
        setFixedSize(kButtonSize, kButtonSize);
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setProperty("role", QString::fromLatin1("glyph"));
    }

    void setColors(const QColor& normal, const QColor& hover)
    {
        m_normal = normal;
        m_hover  = hover;
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override
    {
        QToolButton::paintEvent(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(underMouse() || isDown() ? m_hover : m_normal, 1.5, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = QRectF(rect()).center();
        const qreal   h = 4.0; // 8 px wide
        p.drawLine(c + QPointF(-h, -h), c + QPointF(h, h));
        p.drawLine(c + QPointF(h, -h), c + QPointF(-h, h));
    }

  private:
    QColor m_normal = QColor(0xb5, 0xba, 0xc1);
    QColor m_hover  = QColor(0xf2, 0xf3, 0xf5);
};

// One line of plain text, shortened with "…" to the width it gets (the full text stays available for
// the tooltip), or wrapped onto several lines.
class UploadToast::ElidedLabel : public QLabel
{
  public:
    ElidedLabel(Qt::TextElideMode mode, QWidget* parent)
        : QLabel(parent)
        , m_mode(mode)
    {
        setTextFormat(Qt::PlainText); // file names and server messages, never rich text
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setFullText(const QString& text, bool wrap = false)
    {
        if (text == m_full && wrap == m_wrap)
            return;
        m_full = text;
        if (wrap != m_wrap) {
            m_wrap = wrap;
            setWordWrap(wrap);
            // Long one-line texts never widen the toast; errors wrap instead.
            setSizePolicy(wrap ? QSizePolicy::Preferred : QSizePolicy::Ignored, QSizePolicy::Preferred);
        }
        refresh();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        if (!m_wrap)
            refresh();
    }

  private:
    void refresh()
    {
        QLabel::setText(m_wrap ? m_full : fontMetrics().elidedText(m_full, m_mode, qMax(0, width())));
    }

    Qt::TextElideMode m_mode;
    QString           m_full;
    bool              m_wrap = false;
};

// The alert disc in front of an error, so a failure isn't told by its colour alone. Same shape as the
// alert icon on the chat cards; centred on the first text line.
class UploadToast::AlertGlyph : public QWidget
{
  public:
    explicit AlertGlyph(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(14, qMax(14, fontMetrics().height()));
    }

    void setColors(const QColor& disc, const QColor& mark)
    {
        m_disc = disc;
        m_mark = mark;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box(0.5, (height() - 14) / 2.0 + 0.5, 13.0, 13.0);
        p.setPen(Qt::NoPen);
        p.setBrush(m_disc);
        p.drawEllipse(box);
        const qreal w = box.width() * 0.14;
        p.setBrush(m_mark);
        p.drawRoundedRect(QRectF(box.center().x() - w / 2, box.top() + box.height() * 0.22, w, box.height() * 0.36), w / 2, w / 2);
        p.drawEllipse(QPointF(box.center().x(), box.top() + box.height() * 0.74), w * 0.62, w * 0.62);
    }

  private:
    QColor m_disc = QColor(0xfa, 0x77, 0x7c);
    QColor m_mark = QColor(0x21, 0x23, 0x26);
};

// Exponential moving average of the rate: alpha 0.3 per 250 ms progress tick, scaled to the actual
// gap between updates (Core reports upload progress in 1% steps, which can be seconds apart).
void UploadToast::Speed::sample(qint64 nowMs, double bytes)
{
    if (lastMs < 0 || bytes < lastBytes) {
        startMs   = nowMs;
        lastMs    = nowMs;
        lastBytes = bytes;
        rate      = -1.0;
        return;
    }
    const qint64 dt = nowMs - lastMs;
    if (dt < 200)
        return;
    const double instant = (bytes - lastBytes) * 1000.0 / static_cast<double>(dt);
    const double alpha   = 1.0 - std::pow(0.7, static_cast<double>(dt) / 250.0);
    rate                 = rate < 0.0 ? instant : rate + alpha * (instant - rate);
    lastMs               = nowMs;
    lastBytes            = bytes;
}

bool UploadToast::Speed::hasRate(qint64 nowMs) const
{
    return rate > 0.0 && nowMs - startMs >= 1500;
}

// Shown after 3 s of samples, and not for the last few seconds (no countdown to watch).
qint64 UploadToast::Speed::timeLeftMs(qint64 nowMs, double remaining) const
{
    if (rate <= 0.0 || nowMs - startMs < 3000 || remaining <= 0.0)
        return -1;
    const double ms = remaining * 1000.0 / rate;
    return ms >= 5000.0 && ms < 1.0e10 ? static_cast<qint64>(ms) : -1;
}

UploadToast::UploadToast(Core* core, QWidget* host)
    : QFrame(host)
    , m_core(core)
{
    m_clock.start();
    setObjectName(QStringLiteral("tsmediaUploadToast"));
    setAttribute(Qt::WA_StyledBackground, true);
    setLayoutDirection(Qt::LeftToRight);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(12, 10, 10, 10);
    m_layout->setSpacing(10);

    // Several files: how far the whole send is, and one button to stop it.
    m_header    = new QWidget(this);
    auto* hgrid = new QGridLayout(m_header);
    hgrid->setContentsMargins(0, 0, 0, 0);
    hgrid->setHorizontalSpacing(6);
    hgrid->setVerticalSpacing(5);
    m_headerTitle = new ElidedLabel(Qt::ElideRight, m_header);
    QFont bold    = m_headerTitle->font();
    bold.setBold(true);
    m_headerTitle->setFont(bold);
    m_cancelAll = textButton(i18n::t("Cancel all"), m_header);
    m_cancelAll->setToolTip(i18n::t("Cancel every upload that is still running"));
    QSizePolicy keep = m_cancelAll->sizePolicy();
    keep.setRetainSizeWhenHidden(true); // the header keeps its height when only one upload is left
    m_cancelAll->setSizePolicy(keep);
    connect(m_cancelAll, &QToolButton::clicked, this, &UploadToast::cancelAll);
    m_headerBar    = new ProgressLine(m_header);
    m_headerStatus = new ElidedLabel(Qt::ElideRight, m_header);
    m_headerStatus->setProperty("role", QString::fromLatin1("status"));
    hgrid->addWidget(m_headerTitle, 0, 0);
    hgrid->addWidget(m_cancelAll, 0, 1);
    hgrid->addWidget(m_headerBar, 1, 0, 1, 2);
    hgrid->addWidget(m_headerStatus, 2, 0, 1, 2);
    hgrid->setColumnStretch(0, 1);
    m_header->hide();
    m_layout->addWidget(m_header);
    m_divider = new QFrame(this);
    m_divider->setProperty("role", QString::fromLatin1("divider"));
    m_divider->setFixedHeight(1);
    m_divider->hide();
    m_layout->addWidget(m_divider);

    m_rowsLayout = new QVBoxLayout;
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(10);
    m_layout->addLayout(m_rowsLayout);

    // "+5 more waiting": the rows that don't fit.
    m_more = textButton(QString(), this);
    m_more->setProperty("role", QString::fromLatin1("more"));
    m_more->setFixedHeight(qMax(20, fontMetrics().height() + 4));
    connect(m_more, &QToolButton::clicked, this, [this] {
        m_expanded = !m_expanded;
        relayout();
    });
    m_more->hide();
    m_layout->addWidget(m_more, 0, Qt::AlignLeft);

    hide();
    setHost(host);
}

void UploadToast::setHost(QWidget* host)
{
    if (m_host)
        m_host->removeEventFilter(this);
    m_host = host;
    setParent(host);
    if (host)
        host->installEventFilter(this);
    if (!m_rows.isEmpty())
        relayout();
}

void UploadToast::setDark(bool dark)
{
    if (m_themed && dark == m_dark)
        return;
    m_dark   = dark;
    m_themed = true;
    applyTheme();
}

void UploadToast::applyTheme()
{
    setStyleSheet(QString::fromLatin1(m_dark ? kDarkSheet : kLightSheet));
    // Qt only re-applies a changed style sheet to widgets that were polished already; without this,
    // rows created before the toast was first shown could keep the previous theme's colours.
    ensurePolished();
    const ToastColors c = toastColors(m_dark);
    m_headerBar->setColors(c.track, c.accent);
    for (Row& row : m_rows) {
        row.close->setColors(c.muted, c.text);
        row.alert->setColors(c.error, c.surface);
        row.bar->setColors(c.track, row.state == UploadState::Done ? c.done : c.accent);
    }
}

bool UploadToast::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_host && (event->type() == QEvent::Resize || event->type() == QEvent::Show) && !m_rows.isEmpty())
        relayout(); // how many rows fit depends on the chat's height
    return QFrame::eventFilter(watched, event);
}

// Rows never disappear while the pointer is on the toast (see the removal timer in rowFor); after
// it leaves, each one stays at least two more seconds.
void UploadToast::leaveEvent(QEvent* event)
{
    QFrame::leaveEvent(event);
    for (Row& row : m_rows) {
        if (row.removal->isActive() && row.removal->remainingTime() < 2000)
            row.removal->start(2000);
    }
}

bool UploadToast::pointerInside() const
{
    return isVisible() && rect().contains(mapFromGlobal(QCursor::pos()));
}

int UploadToast::toastWidth() const
{
    return m_host ? qMin(340, qMax(220, m_host->width() - 40)) : 300;
}

// Rows shown at once. Normally at most 4 and never more than about 45% of the chat; after "+N more"
// as many as fit.
int UploadToast::rowLimit(bool expanded) const
{
    if (!m_host)
        return 4;
    const int spacing = m_rowsLayout->spacing();
    const int rowH    = kButtonSize + 5 + 4 + 5 + fontMetrics().height(); // title line, bar, status line
    const int headerH = m_header->isVisibleTo(this) ? m_header->sizeHint().height() + 1 + 2 * m_layout->spacing() : 0; // with the divider
    if (expanded) {
        const int room = m_host->height() - 20 - 20 - headerH - (m_more->height() + m_layout->spacing());
        return qMax(1, (room + spacing) / (rowH + spacing));
    }
    const int room = static_cast<int>(0.45 * m_host->height()) - headerH;
    return qBound(1, (room + spacing) / (rowH + spacing), 4);
}

void UploadToast::reposition()
{
    if (!m_host)
        return;
    setFixedWidth(toastWidth());
    setMaximumHeight(qMax(60, m_host->height() - 20)); // never taller than the chat
    adjustSize();
    // Bottom corner on the reading-end side, clear of the chat's scrollbar.
    const int x = m_host->layoutDirection() == Qt::RightToLeft ? 22 : m_host->width() - width() - 22;
    move(x, qMax(10, m_host->height() - height() - 10));
}

UploadToast::Row& UploadToast::rowFor(int id)
{
    auto it = m_rows.find(id);
    if (it != m_rows.end())
        return it.value();

    const ToastColors c = toastColors(m_dark);
    Row               row;
    row.widget = new QWidget(this);
    auto* grid = new QGridLayout(row.widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(5);

    row.title = new ElidedLabel(Qt::ElideMiddle, row.widget); // keeps the extension visible
    // Left-aligned like the rest of the toast, also when a name is in a right-to-left script.
    row.title->setAlignment(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
    QFont f = row.title->font();
    f.setBold(true);
    row.title->setFont(f);

    row.percent = new QLabel(row.widget);
    row.percent->setTextFormat(Qt::PlainText);
    row.percent->setProperty("role", QString::fromLatin1("status")); // not a literal: the style engine may outlive the DLL
    row.percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.percent->setMinimumWidth(row.percent->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 2); // no jitter while counting
    row.percent->hide();

    row.retry = textButton(i18n::t("Retry"), row.widget);
    row.retry->hide();
    connect(row.retry, &QToolButton::clicked, this, [this, id] {
        // The failed job is replaced by a new one (its row follows through uploadChanged). If that
        // isn't possible now (not connected: Core says so in the chat), the row is refreshed.
        if (m_core && m_core->retryUpload(id) == 0 && m_core->upload(id))
            updateJob(id);
    });

    row.close = new GlyphButton(row.widget);
    row.close->setColors(c.muted, c.text);
    QSizePolicy keep = row.close->sizePolicy();
    keep.setRetainSizeWhenHidden(true); // the title never changes width when the button goes
    row.close->setSizePolicy(keep);
    connect(row.close, &QToolButton::clicked, this, [this, id] {
        const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
        if (job && isRunning(job->state))
            m_core->cancelUpload(id);
        else
            removeRow(id, true);
    });

    row.bar = new ProgressLine(row.widget);
    row.bar->setColors(c.track, c.accent);

    row.alert = new AlertGlyph(row.widget);
    row.alert->setColors(c.error, c.surface);
    row.alert->hide();

    row.status = new ElidedLabel(Qt::ElideRight, row.widget);
    row.status->setProperty("role", QString::fromLatin1("status"));
    row.status->setAlignment(Qt::AlignLeft | Qt::AlignTop); // the first line stays next to the alert glyph

    row.removal = new QTimer(row.widget);
    row.removal->setSingleShot(true);
    connect(row.removal, &QTimer::timeout, this, [this, id] {
        auto r = m_rows.find(id);
        if (r == m_rows.end())
            return;
        if (pointerInside()) {
            r->removal->start(2000); // being read: ask again later
            return;
        }
        removeRow(id, true);
    });

    // The close button sits in the title line only, so a hidden bar leaves no gap; the bar and the
    // status span the whole width and never change size between states.
    grid->addWidget(row.title, 0, 0);
    grid->addWidget(row.percent, 0, 1);
    grid->addWidget(row.retry, 0, 2);
    grid->addWidget(row.close, 0, 3);
    grid->addWidget(row.bar, 1, 0, 1, 4);
    auto* statusLine = new QHBoxLayout;
    statusLine->setContentsMargins(0, 0, 0, 0);
    statusLine->setSpacing(6);
    statusLine->addWidget(row.alert, 0, Qt::AlignTop);
    statusLine->addWidget(row.status, 1);
    grid->addLayout(statusLine, 2, 0, 1, 4);
    grid->setColumnStretch(0, 1);

    m_rowsLayout->addWidget(row.widget);
    m_order.append(id);
    return m_rows.insert(id, row).value();
}

void UploadToast::removeRow(int id, bool dismiss)
{
    auto it = m_rows.find(id);
    if (it == m_rows.end())
        return;
    it->widget->hide();
    it->widget->deleteLater(); // may be called from one of its own buttons
    m_rows.erase(it);
    m_order.removeAll(id);
    // Core forgets the finished job (a pasted image kept for a retry is deleted with it).
    if (dismiss && m_core)
        m_core->dismissUpload(id);
    relayout();
}

void UploadToast::cancelAll()
{
    if (!m_core)
        return;
    QList<int> ids = m_tally.keys();
    std::sort(ids.begin(), ids.end());
    for (int id : qAsConst(ids)) {
        const UploadJob* job = m_core->upload(id);
        if (job && isRunning(job->state))
            m_core->cancelUpload(id);
    }
}

void UploadToast::updateJob(int id)
{
    const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
    if (!job) {
        // Gone from Core: dismissed, sent again as a new job, or expired. Sent files still count in
        // the header until the whole send is over.
        auto t = m_tally.find(id);
        if (t != m_tally.end() && t->state != UploadState::Done)
            m_tally.erase(t);
        if (m_rows.contains(id))
            removeRow(id, false);
        else if (!m_rows.isEmpty())
            relayout();
        return;
    }

    if (!m_tally.contains(id)) {
        // A new send after the previous one is over starts a new count.
        bool busy = false;
        for (const Tally& t : qAsConst(m_tally))
            busy = busy || isActive(t.state);
        if (!busy) {
            m_tally.clear();
            m_totalSpeed.reset();
            m_expanded = false;
        }
    }
    Tally& tally = m_tally[id];
    tally.size   = job->size;
    tally.state  = job->state;
    if (job->state == UploadState::Uploading)
        tally.sent = static_cast<quint64>(qBound(0.0, job->progress, 1.0) * static_cast<double>(job->size));
    else if (job->uploaded || job->state == UploadState::Posting || job->state == UploadState::Done)
        tally.sent = job->size;

    Row& row = rowFor(id);
    fillRow(id, row, *job);
    relayout();
}

void UploadToast::fillRow(int id, Row& row, const UploadJob& job)
{
    const ToastColors c       = toastColors(m_dark);
    const qint64      now     = m_clock.elapsed();
    const bool        changed = !row.known || row.state != job.state || row.waiting != job.waiting;
    row.state                 = job.state;
    row.waiting               = job.waiting;
    row.known                 = true;

    // "Pasted image" (with its size once probed) or the file's name.
    const QString name  = displayNameFor(job);
    QString       title = name;
    if (job.pasted && job.info.width > 0 && job.info.height > 0)
        title += dot() + i18n::t("%1 × %2").arg(job.info.width).arg(job.info.height);
    row.title->setFullText(title);
    row.title->setToolTip(plainToolTip(title, true));

    QString status;  // what the line shows (shortened to fit)
    QString spoken;  // the full status: tooltip and accessible name
    QString state;   // style: "", "done" or "error"
    bool    percent    = false;
    int     spokenStep = -1; // quarters of an upload
    switch (job.state) {
    case UploadState::Preparing:
        // Waiting for a free upload slot (a still bar), or probing the file and uploading its preview.
        status = !job.message.isEmpty() ? job.message : job.waiting ? i18n::t("Waiting to upload…") : i18n::t("Preparing…");
        if (job.waiting)
            row.bar->setProgress(0.0);
        else
            row.bar->setBusy();
        row.speed.reset();
        break;
    case UploadState::Uploading: {
        // The bar and the percentage say "uploading"; the line has the rest in the shared order:
        // rate, amounts, time left ("1.2 MB/s · 4.5 MB of 10.0 MB · 2 min left").
        const double progress = qBound(0.0, job.progress, 1.0);
        const int    value    = qBound(0, static_cast<int>(std::floor(progress * 100.0)), 100); // 100% only when complete
        row.bar->setProgress(progress);
        row.percent->setText(i18n::t("%1%").arg(value));
        percent    = true;
        spokenStep = value / 25;
        QStringList parts;
        if (job.size > 0) {
            const double sent = progress * static_cast<double>(job.size);
            row.speed.sample(now, sent);
            if (row.speed.hasRate(now))
                parts << formatSpeed(row.speed.rate);
            parts << formatProgress(static_cast<quint64>(sent), job.size);
            const qint64 left = row.speed.timeLeftMs(now, static_cast<double>(job.size) - sent);
            if (left >= 0)
                parts << formatTimeLeft(left);
        }
        status = parts.isEmpty() ? i18n::t("Uploading…") : parts.join(dot());
        spoken = i18n::t("Uploading… %1%").arg(value) + (parts.isEmpty() ? QString() : dot() + parts.join(dot()));
        break;
    }
    case UploadState::Posting:
        status = !job.message.isEmpty() ? job.message : job.waiting ? i18n::t("Waiting for earlier files…") : i18n::t("Posting to chat…");
        row.bar->setProgress(1.0);
        break;
    case UploadState::Done:
        status = i18n::t("Sent");
        state  = QString::fromLatin1("done");
        row.bar->setProgress(1.0);
        break;
    case UploadState::Failed:
        // Core's text says what happened and what to do; the alert glyph marks it as an error.
        status = job.message.isEmpty() ? uploadErrorText(MediaError::Other) : job.message;
        state  = QString::fromLatin1("error");
        break;
    case UploadState::Canceled:
        status = i18n::t("Canceled");
        break;
    }
    if (spoken.isEmpty())
        spoken = status;

    const bool failed = job.state == UploadState::Failed;
    row.status->setFullText(status, failed);
    row.status->setToolTip(plainToolTip(spoken, false));
    // Screen readers hear about new steps and each quarter of an upload, not about every percent
    // (setAccessibleName announces the change itself).
    if (changed || spokenStep != row.spokenStep) {
        row.spokenStep = spokenStep;
        row.status->setAccessibleName(spoken);
    }
    if (state != row.styleState) {
        row.styleState = state;
        setStyleState(row.status, state);
    }
    row.alert->setVisible(failed);
    row.percent->setVisible(percent);
    row.bar->setColors(c.track, job.state == UploadState::Done ? c.done : c.accent);
    row.bar->setVisible(!failed && job.state != UploadState::Canceled);

    const bool running  = isRunning(job.state);
    const bool canRetry = failed && m_core && m_core->canRetryUpload(id);
    row.retry->setVisible(canRetry);
    row.close->setVisible(job.state != UploadState::Posting); // a chat message can't be called back
    row.close->setToolTip(running ? i18n::t("Cancel upload") : i18n::t("Dismiss"));

    if (!changed)
        return;
    // The icon-only button gets a name with the file in it.
    row.retry->setAccessibleName(i18n::t("Retry sending “%1”").arg(name));
    row.close->setAccessibleName(running ? i18n::t("Cancel upload of “%1”").arg(name) : i18n::t("Dismiss “%1”").arg(name));
    // Finished rows go after a while (the timer never removes one while the pointer is on the toast).
    switch (job.state) {
    case UploadState::Done:
        row.removal->start(1500); // the message appearing in the chat is the real confirmation
        break;
    case UploadState::Canceled:
        row.removal->start(2500);
        break;
    case UploadState::Failed:
        // Errors stay long enough to be read, at least as long as Windows keeps notifications.
        row.removal->start(qMax(canRetry ? 15000 : 10000, ui::notificationDurationMs()));
        break;
    default:
        row.removal->stop();
        break;
    }
}

void UploadToast::updateHeader()
{
    int     count = 0, done = 0, running = 0;
    bool    active = false;
    quint64 total = 0, sent = 0, transferred = 0;
    for (const Tally& t : qAsConst(m_tally)) {
        transferred += t.sent; // only grows: the rate of the whole send
        if (t.state == UploadState::Canceled)
            continue;
        ++count;
        done += t.state == UploadState::Done ? 1 : 0;
        running += isRunning(t.state) ? 1 : 0;
        active = active || isActive(t.state);
        if (t.state == UploadState::Failed)
            continue;
        total += t.size;
        sent += qMin(t.sent, t.size);
    }
    const bool show = count >= 2 && active;
    m_header->setVisible(show);
    m_divider->setVisible(show);
    if (!show) {
        m_totalSpeed.reset();
        return;
    }

    const qint64 now = m_clock.elapsed();
    m_totalSpeed.sample(now, static_cast<double>(transferred));
    const double fraction = total > 0 ? static_cast<double>(sent) / static_cast<double>(total) : 0.0;
    const int    percent  = qBound(0, static_cast<int>(std::floor(fraction * 100.0)), 100);
    // "Sending 9 files" / "41% · 3 of 9 sent · 2 min left"
    const QString title = i18n::t("Sending %1 files").arg(count);
    m_headerTitle->setFullText(title);
    QStringList parts{i18n::t("%1%").arg(percent), i18n::t("%1 of %2 sent").arg(done).arg(count)};
    const qint64 left = m_totalSpeed.timeLeftMs(now, static_cast<double>(total - sent));
    if (left >= 0)
        parts << formatTimeLeft(left);
    m_headerStatus->setFullText(parts.join(dot()));
    const QString spoken = title + dot() + parts.at(1); // changes when a file is done, not every percent
    if (m_headerStatus->accessibleName() != spoken)
        m_headerStatus->setAccessibleName(spoken);
    m_headerBar->setProgress(fraction);
    m_cancelAll->setVisible(running >= 2);
}

void UploadToast::relayout()
{
    updateHeader();
    if (m_rows.isEmpty()) {
        m_more->hide();
        hide();
        return;
    }

    // Failed rows first (they need attention), then the ones at work or just finished, then those
    // waiting for their turn; each group oldest first.
    auto group = [this](int id) {
        const Row& r = m_rows[id];
        if (r.state == UploadState::Failed)
            return 0;
        if (r.state == UploadState::Preparing && r.waiting)
            return 2;
        return 1;
    };
    QList<int> order = m_rows.keys();
    std::sort(order.begin(), order.end(), [&group](int a, int b) {
        const int ga = group(a);
        const int gb = group(b);
        return ga != gb ? ga < gb : a < b;
    });
    // Not QList::operator!=: for int on 32-bit builds it goes through MSVC's deprecated checked_array_iterator.
    if (order.size() != m_order.size() || !std::equal(order.cbegin(), order.cend(), m_order.cbegin())) {
        for (int id : qAsConst(order))
            m_rowsLayout->removeWidget(m_rows[id].widget);
        for (int id : qAsConst(order))
            m_rowsLayout->addWidget(m_rows[id].widget);
        m_order = order;
    }

    const int limit       = rowLimit(m_expanded);
    int       hidden      = 0;
    bool      onlyWaiting = true;
    for (int i = 0; i < order.size(); ++i) {
        const Row& r       = m_rows[order.at(i)];
        const bool visible = i < limit;
        r.widget->setVisible(visible);
        if (!visible) {
            ++hidden;
            onlyWaiting = onlyWaiting && r.state == UploadState::Preparing && r.waiting;
        }
    }
    if (m_expanded && order.size() > rowLimit(false)) {
        m_more->setText(i18n::t("Show fewer"));
        m_more->show();
    } else if (hidden > 0) {
        m_more->setText(onlyWaiting ? i18n::t("+%1 more waiting").arg(hidden) : i18n::t("+%1 more").arg(hidden));
        m_more->show();
    } else {
        m_more->hide();
    }

    if (!m_themed)
        setDark(m_dark);
    show();
    raise();
    reposition();
}
