#include "uploadtoast.h"

#include <QBasicTimer>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QTimer>
#include <QTimerEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include "core.h"
#include "i18n.h"
#include "previewrenderer.h"

// Thin rounded progress line; also has an indeterminate mode for the preparing phase.
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
        m_busy = false;
        m_timer.stop();
        m_value = qBound(0.0, value, 1.0);
        update();
    }

    void setBusy()
    {
        if (m_busy)
            return;
        m_busy  = true;
        m_phase = 0.0;
        if (isVisible())
            m_timer.start(30, this);
        update();
    }

    void setColor(const QColor& color)
    {
        m_color = color;
        update();
    }

  protected:
    void showEvent(QShowEvent*) override
    {
        if (m_busy)
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
        p.setBrush(QColor(0x4e, 0x50, 0x58));
        p.drawRoundedRect(r, radius, radius);

        QRectF fill;
        if (m_busy) {
            const qreal segment = r.width() * 0.3;
            fill                = QRectF(-segment + (r.width() + segment) * m_phase, 0, segment, r.height()).intersected(r);
        } else {
            fill = QRectF(0, 0, r.width() * m_value, r.height());
        }
        if (fill.width() <= 0.0)
            return;
        p.setBrush(m_color);
        p.drawRoundedRect(fill, radius, radius);
    }

  private:
    double      m_value = 0.0;
    double      m_phase = 0.0;
    bool        m_busy  = false;
    QColor      m_color = QColor(0x58, 0x65, 0xf2);
    QBasicTimer m_timer;
};

namespace {

QIcon closeIcon()
{
    auto make = [](const QColor& color) {
        QPixmap pixmap(32, 32);
        pixmap.setDevicePixelRatio(2.0);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4.5, 4.5), QPointF(11.5, 11.5));
        p.drawLine(QPointF(11.5, 4.5), QPointF(4.5, 11.5));
        return pixmap;
    };
    QIcon icon;
    icon.addPixmap(make(QColor(0xb5, 0xba, 0xc1)), QIcon::Normal);
    icon.addPixmap(make(QColor(0xf2, 0xf3, 0xf5)), QIcon::Active);
    return icon;
}

bool isRunning(UploadState state)
{
    return state == UploadState::Preparing || state == UploadState::Uploading;
}

// Qt shows a tooltip as rich text when it looks like HTML; file names and server messages are text.
QString plainToolTip(const QString& text, bool singleLine)
{
    if (text.isEmpty())
        return text;
    return (singleLine ? QStringLiteral("<p style='white-space:pre'>%1</p>") : QStringLiteral("<p>%1</p>")).arg(text.toHtmlEscaped());
}

} // namespace

UploadToast::UploadToast(Core* core, QWidget* host)
    : QFrame(host)
    , m_core(core)
{
    m_clock.start();
    setObjectName(QStringLiteral("tsmediaUploadToast"));
    setAttribute(Qt::WA_StyledBackground, true);
    setLayoutDirection(Qt::LeftToRight);
    // fromLatin1, never QStringLiteral: the toast lives in TeamSpeak's chat and shares its
    // QStyleSheetStyle when TeamSpeak uses style sheets. That style's parser keeps the last text it
    // parsed until TeamSpeak destroys it at exit, after this DLL is unloaded, and ~QString on literal
    // data of an unloaded DLL crashes TeamSpeak.
    setStyleSheet(QString::fromLatin1(
        "#tsmediaUploadToast { background: rgba(32,34,37,238); border: 1px solid rgba(255,255,255,30); border-radius: 8px; }"
        "#tsmediaUploadToast QLabel { color: #f2f3f5; background: transparent; }"
        "#tsmediaUploadToast QLabel[role=\"status\"] { color: #b5bac1; }"
        "#tsmediaUploadToast QLabel[role=\"status\"][state=\"error\"] { color: #f23f43; }"
        "#tsmediaUploadToast QLabel[role=\"status\"][state=\"done\"] { color: #23a55a; }"
        "#tsmediaUploadToast QToolButton { background: transparent; border: none; border-radius: 4px; padding: 2px; }"
        "#tsmediaUploadToast QToolButton:hover { background: rgba(255,255,255,24); }"
        "QToolTip { color: #f2f3f5; background: #111214; border: 1px solid #3f4147; padding: 4px 6px; }"));

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(12, 10, 10, 10);
    m_layout->setSpacing(10);
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
    if (!m_rows.isEmpty()) {
        show();
        raise();
        reposition();
    }
}

bool UploadToast::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_host && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        reposition();
    return QFrame::eventFilter(watched, event);
}

int UploadToast::toastWidth() const
{
    return m_host ? qMin(340, qMax(220, m_host->width() - 40)) : 300;
}

void UploadToast::reposition()
{
    if (!m_host)
        return;
    setFixedWidth(toastWidth());
    adjustSize();
    // Bottom corner on the reading-end side, clear of the chat's scrollbar.
    const int x = m_host->layoutDirection() == Qt::RightToLeft ? 22 : m_host->width() - width() - 22;
    move(x, m_host->height() - height() - 10);
}

UploadToast::Row& UploadToast::rowFor(int id)
{
    auto it = m_rows.find(id);
    if (it != m_rows.end())
        return it.value();

    Row row;
    row.widget = new QWidget(this);
    auto* grid = new QGridLayout(row.widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(5);

    row.title = new QLabel(row.widget);
    row.title->setTextFormat(Qt::PlainText); // a file name, never rich text
    row.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Left-aligned like the rest of the toast, also when a name is in a right-to-left script.
    row.title->setAlignment(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
    QFont f = row.title->font();
    f.setBold(true);
    row.title->setFont(f);

    row.status = new QLabel(row.widget);
    row.status->setTextFormat(Qt::PlainText); // may quote a file name or a server message
    row.status->setProperty("role", QString::fromLatin1("status")); // not a literal: the style engine may outlive the DLL

    row.bar = new ProgressLine(row.widget);

    row.close = new QToolButton(row.widget);
    row.close->setIcon(closeIcon());
    row.close->setIconSize(QSize(16, 16));
    row.close->setAutoRaise(true);
    row.close->setCursor(Qt::PointingHandCursor);
    row.close->setFocusPolicy(Qt::NoFocus);
    connect(row.close, &QToolButton::clicked, this, [this, id] {
        const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
        if (job && isRunning(job->state))
            m_core->cancelUpload(id);
        else
            removeRow(id);
    });

    grid->addWidget(row.title, 0, 0);
    grid->addWidget(row.close, 0, 1, 2, 1, Qt::AlignTop);
    grid->addWidget(row.bar, 1, 0);
    grid->addWidget(row.status, 2, 0, 1, 2);
    grid->setColumnStretch(0, 1);

    m_layout->addWidget(row.widget);
    return m_rows.insert(id, row).value();
}

void UploadToast::removeRow(int id)
{
    auto it = m_rows.find(id);
    if (it == m_rows.end())
        return;
    it->widget->deleteLater();
    m_rows.erase(it);
    if (m_rows.isEmpty())
        hide();
    else
        QTimer::singleShot(0, this, [this] { reposition(); });
}

void UploadToast::updateJob(int id)
{
    const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
    if (!job) {
        removeRow(id);
        return;
    }

    Row& row = rowFor(id);

    const QString name = displayFileName(QFileInfo(job->sourcePath).fileName());
    row.title->setText(row.title->fontMetrics().elidedText(name, Qt::ElideMiddle, toastWidth() - 60));
    row.title->setToolTip(plainToolTip(name, true));

    QString state;
    QString status;
    switch (job->state) {
    case UploadState::Preparing:
        // Probing the file and uploading its preview.
        status            = i18n::t("Preparing…");
        row.uploadStartMs = -1;
        row.bar->setBusy();
        break;
    case UploadState::Uploading: {
        const double progress = qBound(0.0, job->progress, 1.0);
        if (row.uploadStartMs < 0) {
            row.uploadStartMs       = m_clock.elapsed();
            row.uploadStartProgress = progress;
        }
        row.bar->setProgress(progress);
        status = i18n::t("Uploading… %1%").arg(qRound(progress * 100));
        if (job->size > 0) {
            const quint64 sent = static_cast<quint64>(progress * static_cast<double>(job->size));
            status += QStringLiteral("  ·  ") + formatSize(sent) + QStringLiteral(" / ") + formatSize(job->size);
            const qint64 elapsed = m_clock.elapsed() - row.uploadStartMs;
            if (elapsed > 1500 && progress > row.uploadStartProgress) {
                const double bytesPerSecond = (progress - row.uploadStartProgress) * static_cast<double>(job->size) * 1000.0 / static_cast<double>(elapsed);
                status += QStringLiteral("  ·  ") + i18n::t("%1/s").arg(formatSize(static_cast<quint64>(bytesPerSecond)));
            }
        }
        break;
    }
    case UploadState::Posting:
        status = i18n::t("Posting to chat…");
        row.bar->setProgress(1.0);
        break;
    case UploadState::Done:
        status = i18n::t("Sent");
        state  = QStringLiteral("done");
        row.bar->setProgress(1.0);
        row.bar->setColor(QColor(0x23, 0xa5, 0x5a));
        break;
    case UploadState::Failed:
        status = job->message.isEmpty() ? i18n::t("Upload failed") : job->message;
        state  = QStringLiteral("error");
        break;
    case UploadState::Canceled:
        status = i18n::t("Canceled");
        break;
    }

    const bool failed = job->state == UploadState::Failed;
    row.status->setText(status);
    row.status->setToolTip(plainToolTip(status, false));
    row.status->setWordWrap(failed);
    // Long one-line statuses are clipped instead of widening the toast; errors wrap.
    row.status->setSizePolicy(failed ? QSizePolicy::Preferred : QSizePolicy::Ignored, QSizePolicy::Preferred);
    row.status->setProperty("state", state);
    row.status->style()->unpolish(row.status);
    row.status->style()->polish(row.status);

    const bool running = isRunning(job->state);
    row.close->setVisible(running || failed || job->state == UploadState::Canceled);
    row.close->setToolTip(running ? i18n::t("Cancel upload") : i18n::t("Dismiss"));
    row.bar->setVisible(!failed && job->state != UploadState::Canceled);

    if (!running && job->state != UploadState::Posting && !row.removalScheduled) {
        row.removalScheduled = true;
        const int delay      = failed ? 8000 : 2500;
        QTimer::singleShot(delay, this, [this, id] { removeRow(id); });
    }

    show();
    raise();
    reposition();
}
